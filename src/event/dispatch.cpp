#include "event/dispatch.hpp"
#include "util/util.hpp"
#include "log/log.hpp"
#include "cmd/cmd.hpp"
#include "agent/agent.hpp"

#include <cstdio>
#include <queue>
#include <stack>
#include <map>
#include <string>
#include <stdexcept>

#include "platform/thread/thread.hpp"

namespace msa { namespace event {

	static const PluginHooks HOOKS = {
		#define MSA_MODULE_HOOK(retspec, name, ...)		name,
		#include "event/hooks.hpp"
		#undef MSA_MODULE_HOOK
	};

	typedef std::chrono::high_resolution_clock chrono_clock;
	typedef chrono_clock::time_point chrono_time;
	
	class Timer
	{
		public:
			int16_t id;
			std::chrono::milliseconds period;
			std::chrono::high_resolution_clock::time_point last_fired;
			bool recurring;
			IArgs *event_args;
			Topic event_topic;
			Timer(std::chrono::milliseconds period, Topic topic, const IArgs &args, bool recurring) :
				id(0),
				period(period),
				last_fired(chrono_clock::now()),
				recurring(recurring),
				event_args(args.copy()),
				event_topic(topic)
			{}
			Timer(const Timer &other) :
				id(other.id),
				period(other.period),
				last_fired(other.last_fired),
				recurring(other.recurring),
				event_args(other.event_args->copy()),
				event_topic(other.event_topic)
			{}
			~Timer()
			{
				delete event_args;
			}
			Timer &operator=(const Timer &other)
			{
				delete event_args;
				event_args = other.event_args->copy();
				id = other.id;
				period = other.period;
				last_fired = other.last_fired;
				event_topic = other.event_topic;
				return *this;
			}
	};

	typedef struct handler_context_type {
		const Event *event;
		EventHandler handler_func;
		HandlerSync *sync;
		bool running;
		msa::thread::Thread thread;
		bool reap_in_handler;
	} HandlerContext;

	struct event_dispatch_context_type {
		msa::thread::Thread edt;
		msa::thread::Mutex queue_mutex;
		HandlerContext *current_handler;
		std::priority_queue<const Event *> queue;
		std::map<Topic, EventHandler> handlers;
		std::stack<HandlerContext *> interrupted;
		int sleep_time;
		std::chrono::milliseconds tick_resolution;
		chrono_time last_tick_time;
		std::map<int16_t, Timer*> timers;
		msa::thread::Mutex timers_mutex;
		std::vector<msa::cmd::Command *> commands;
	};

	static int create_event_dispatch_context(EventDispatchContext **event);
	static int dispose_event_dispatch_context(EventDispatchContext *event);
	static void read_config(msa::Handle hdl, const msa::cfg::Section &config);
	static void *event_start(void *args);
	
	static void push_event(msa::Handle msa, const Event *e);

	static void *edt_start(void *args);
	static void edt_run(msa::Handle hdl);
	static void edt_cleanup(msa::Handle hdl);
	static const Event *edt_poll_event_queue(msa::Handle hdl);
	static void edt_interrupt_handler(msa::Handle hdl);
	static void edt_spawn_handler(msa::Handle hdl, const Event *e);
	static void edt_dispatch_event(msa::Handle hdl, const Event *e);
	static void edt_fire_timers(msa::Handle hdl, chrono_time now);
	static void dispose_handler_context(HandlerContext *ctx, bool wait);

	static void cmd_timer(msa::Handle hdl, const msa::cmd::ParamList &params, HandlerSync *const sync);
	static void cmd_deltimer(msa::Handle hdl, const msa::cmd::ParamList &params, HandlerSync *const sync);

	extern int init(msa::Handle hdl, const msa::cfg::Section &config)
	{
		int create_status = create_event_dispatch_context(&hdl->event);
		if (create_status != 0)
		{
			msa::log::error(hdl, "Could not create event context (error " + std::to_string(create_status) + ")");
			return create_status;
		}
		
		// read config
		try
		{
			read_config(hdl, config);
		}
		catch (const msa::cfg::config_error &e)
		{
			msa::log::error(hdl, "Could not read event config: " + std::string(e.what()));
			return 1;
		}

		create_status = msa::thread::create(&hdl->event->edt, NULL, edt_start, hdl, "edt");
		if (create_status != 0)
		{
			msa::log::error(hdl, "Could not create event dispatch thread (error " + std::to_string(create_status) + ")");
			msa::thread::mutex_destroy(&hdl->event->queue_mutex);
			return create_status;
		}
		return 0;
	}

	extern int quit(msa::Handle msa)
	{
		if (msa->status == msa::Status::CREATED && msa->event != NULL)
		{
			// this shouldn't happen, but if we get here, it's because
			// the event handle was inited but the EDT was not started.
			// We can just destroy the mutex and delete everything immediately
			msa::log::warn(msa, "EDT has not yet set status to RUNNING! Killing anyways");
			msa::thread::mutex_destroy(&msa->event->queue_mutex);
			dispose_event_dispatch_context(msa->event);
			return 0;
		}
		// if the quit was initiated by the current event thread,
		// we must mark it as such so that the EDT knows not to
		// wait on it (since this thread also joins on the EDT,
		// this would cause deadlock)
		if (msa::thread::self() == msa->event->current_handler->thread)
		{
			set_handler_syscall_origin(msa->event->current_handler->sync);
		}
		msa->status = msa::Status::STOP_REQUESTED;
		msa::log::trace(msa, "Joining on EDT");
		msa::thread::join(msa->event->edt, NULL);
		msa::log::trace(msa, "EDT joined");
		dispose_event_dispatch_context(msa->event);
		return 0;
	}
	
	extern int setup(msa::Handle hdl)
	{
		EventDispatchContext *ctx = hdl->event;
		for (size_t i = 0; i < ctx->commands.size(); i++)
		{
			msa::cmd::register_command(hdl, ctx->commands[i]);
		}
		return 0;
	}
	
	extern int teardown(msa::Handle hdl)
	{
		EventDispatchContext *ctx = hdl->event;
		for (size_t i = 0; i < ctx->commands.size(); i++)
		{
			msa::cmd::unregister_command(hdl, ctx->commands[i]);
		}
		return 0;
	}

	extern const PluginHooks *get_plugin_hooks()
	{
		return &HOOKS;
	}

	extern void subscribe(msa::Handle msa, Topic t, EventHandler handler)
	{
		msa->event->handlers[t] = handler;
	}

	extern void unsubscribe(msa::Handle msa, Topic t, EventHandler UNUSED(handler))
	{
		msa->event->handlers[t] = NULL;
	}

	extern void generate(msa::Handle msa, Topic t, const IArgs &args)
	{
		const Event *e = create(t, args);
		msa::log::debug(msa, "Pushed a " + topic_str(t) + " event");
		push_event(msa, e);
	}

	extern int16_t schedule(msa::Handle msa, time_t timestamp, const Topic topic, const IArgs &args)
	{
		time_t ref_time = time(NULL);
		if (ref_time >= timestamp)
		{
			return -1;
		}
		return delay(msa, std::chrono::seconds(timestamp), topic, args);
	}

	extern int16_t delay(msa::Handle msa, std::chrono::milliseconds delay, const Topic topic, const IArgs &args)
	{
		Timer *t = new Timer(delay, topic, args, false);
		msa::thread::mutex_lock(&msa->event->timers_mutex);
		t->id = msa->event->timers.size();
		msa->event->timers[t->id] = t;
		msa::thread::mutex_unlock(&msa->event->timers_mutex);
		msa::log::debug(msa, "Scheduled a " + topic_str(t->event_topic) + " event to fire in " + std::to_string(delay.count()) + "ms (id = " + std::to_string(t->id) + ")");
		return t->id;
	}
	
	extern int16_t add_timer(msa::Handle msa, std::chrono::milliseconds period, const Topic topic, const IArgs &args)
	{
		Timer *t = new Timer(period, topic, args, true);
		msa::thread::mutex_lock(&msa->event->timers_mutex);
		t->id = msa->event->timers.size();
		msa->event->timers[t->id] = t;
		msa::thread::mutex_unlock(&msa->event->timers_mutex);
		msa::log::debug(msa, "Scheduled a " + topic_str(t->event_topic) + " event to fire every " + std::to_string(period.count()) + "ms (id = " + std::to_string(t->id) + ")");
		return t->id;
	}

	extern void remove_timer(msa::Handle msa, int16_t id)
	{
		EventDispatchContext *ctx = msa->event;
		msa::thread::mutex_lock(&ctx->timers_mutex);
		if (ctx->timers.find(id) == ctx->timers.end())
		{
			msa::thread::mutex_unlock(&ctx->timers_mutex);
			throw std::logic_error("no timer with ID: " + std::to_string(id));
		}
		Timer *t = ctx->timers[id];
		ctx->timers.erase(id);
		msa::thread::mutex_unlock(&ctx->timers_mutex);
		delete t;
		msa::log::debug(msa, "Removed timer ID " + std::to_string(id));
		return;
	}

	extern void get_timers(msa::Handle msa, std::vector<int16_t> &list)
	{
		EventDispatchContext *ctx = msa->event;
		msa::thread::mutex_lock(&ctx->timers_mutex);
		std::map<int16_t, Timer*>::const_iterator iter;
		for (iter = ctx->timers.begin(); iter != ctx->timers.end(); iter++)
		{
			list.push_back(iter->first);
		}
		msa::thread::mutex_unlock(&ctx->timers_mutex);
	}

	static int create_event_dispatch_context(EventDispatchContext **event)
	{
		EventDispatchContext *edc = new EventDispatchContext;
		msa::thread::mutex_init(&edc->queue_mutex, NULL);
		msa::thread::mutex_init(&edc->timers_mutex, NULL);
		edc->current_handler = NULL;
		edc->last_tick_time = chrono_time::min();
		edc->commands.push_back(new msa::cmd::Command("TIMER", "It schedules a command to execute in the future", "time-ms command", "r", cmd_timer));
		edc->commands.push_back(new msa::cmd::Command("DELTIMER", "It deletes a timer", "timer-id", cmd_deltimer)),
		*event = edc;
		return 0;
	}

	static int dispose_event_dispatch_context(EventDispatchContext *event)
	{
		// it would seem that we should destory our mutexes here, but
		// that is instead handled in the edt_cleanup function.
		auto iter = event->commands.begin();
		while (iter != event->commands.end())
		{
			msa::cmd::Command *c = *iter;
			iter = event->commands.erase(iter);
			delete c;
		}
		delete event;
		return 0;
	}

	static void read_config(msa::Handle hdl, const msa::cfg::Section &config)
	{
		config.check_range("IDLE_SLEEP_TIME", 1, 1000, false);
		int sleep_time = config.get_or("IDLE_SLEEP_TIME", 10);
		config.check_range("TICK_RESOLUTION", sleep_time, 1000, false);
		int tick_res = config.get_or("TICK_RESOLUTION", 10);
		hdl->event->sleep_time = sleep_time;
		hdl->event->tick_resolution = std::chrono::milliseconds(tick_res);
	}

	static void *edt_start(void *args)
	{
		msa::Handle hdl = (msa::Handle) args;
		hdl->status = msa::Status::RUNNING;
		while (hdl->status != msa::Status::STOP_REQUESTED)
		{
			edt_run(hdl);
			msa::util::sleep_milli(hdl->event->sleep_time);
		}
		edt_cleanup(hdl);
		return NULL;
	}

	static void edt_cleanup(msa::Handle hdl)
	{
		EventDispatchContext *ctx = hdl->event;
		if (ctx->current_handler != NULL)
		{
			// if the syscall that caused the EDT to enter cleanup
			// is from the current event handler, do not wait for it
			// to complete before freeing its resources
			bool wait = !handler_syscall_origin(ctx->current_handler->sync);
			dispose_handler_context(ctx->current_handler, wait);
		}
		while (!ctx->interrupted.empty())
		{
			HandlerContext *intr_ctx = ctx->interrupted.top();
			ctx->interrupted.pop();
			dispose_handler_context(intr_ctx, true);
		}
		msa::thread::mutex_destroy(&hdl->event->queue_mutex);
		msa::thread::mutex_destroy(&hdl->event->timers_mutex);
		while (!hdl->event->queue.empty())
		{
			const Event *e = hdl->event->queue.top();
			hdl->event->queue.pop();
			delete e;
		}
		auto timer_iter = hdl->event->timers.begin();
		while (timer_iter != hdl->event->timers.end())
		{
			Timer *t = timer_iter->second;
			timer_iter = hdl->event->timers.erase(timer_iter);
			delete t;
		}
	}

	static void edt_run(msa::Handle hdl) {
		// check event_queue, decide if we want the current top
		const Event *e = edt_poll_event_queue(hdl);
		if (e != NULL)
		{
			msa::log::debug(hdl, "Dispatching " + topic_str(e->topic) + " event");
			edt_dispatch_event(hdl, e);
		}
		
		EventDispatchContext *edc = hdl->event;
		// check if current task has finished
		if (edc->current_handler != NULL && !(edc->current_handler->running))
		{
			dispose_handler_context(edc->current_handler, false);
			edc->current_handler = NULL;
		}
		// if current task is clear, load up the next one that has been interrupted
		if (edc->current_handler == NULL && !edc->interrupted.empty())
		{
			HandlerContext *ctx = edc->interrupted.top();
			edc->interrupted.pop();
			resume_handler(ctx->sync);
			edc->current_handler = ctx;
		}
		// TODO: Add a synthetic event for when queue has emptied and no events

		// check if we need to do timing tasks
		chrono_time now = chrono_clock::now();
		if (edc->last_tick_time + edc->tick_resolution <= now)
		{
			edc->last_tick_time = now;
			edt_fire_timers(hdl, now);
		}
	}

	static void edt_fire_timers(msa::Handle hdl, chrono_time now)
	{
		EventDispatchContext *ctx = hdl->event;
		msa::thread::mutex_lock(&ctx->timers_mutex);
		std::map<int16_t, Timer*>::iterator iter = ctx->timers.begin();
		while (iter != ctx->timers.end())
		{
			Timer *t = iter->second;
			if (t->last_fired + t->period <= now)
			{
				int16_t id = iter->first;
				generate(hdl, t->event_topic, *t->event_args);
				msa::log::debug(hdl, "Fired timer " + std::to_string(id));
				if (t->recurring)
				{
					t->last_fired = now;
					iter++;
				}
				else
				{
					iter = ctx->timers.erase(iter);
					delete t;
					msa::log::debug(hdl, "Completed and removed timer " + std::to_string(id));
				}
			}
			else
			{
				iter++;
			}
		}
		msa::thread::mutex_unlock(&ctx->timers_mutex);
	}

	static const Event *edt_poll_event_queue(msa::Handle hdl)
	{
		const Event *e = NULL;
		msa::thread::mutex_lock(&hdl->event->queue_mutex);
		if (!hdl->event->queue.empty())
		{
			e = hdl->event->queue.top();
			// if we have a current event, check to see if we should replace it
			// with the event on the queue
			if (hdl->event->current_handler != NULL)
			{
				HandlerContext *ctx = hdl->event->current_handler;
				uint8_t cur_prior = get_priority(ctx->event);
				uint8_t new_prior = get_priority(e);
				if (cur_prior >= new_prior)
				{
					e = NULL;
				}
				// otherwise finish our current task before handling next event
			}
		}
		if (e != NULL)
		{
			hdl->event->queue.pop();
		}
		msa::thread::mutex_unlock(&hdl->event->queue_mutex);
		return e;
	}

	static void edt_interrupt_handler(msa::Handle hdl)
	{
		HandlerContext *ctx = hdl->event->current_handler;
		suspend_handler(ctx->sync);
		// wait till it stops
		while (!handler_suspended(ctx->sync)) {
			// we just wait here until we can go
			msa::util::sleep_milli(hdl->event->sleep_time);
			// TODO: Force handler to stop if it takes too long
		}
		// okay now put it on the stack
		hdl->event->interrupted.push(ctx);
		// and clear the current
		hdl->event->current_handler = NULL;
	}

	static void edt_spawn_handler(msa::Handle hdl, const Event *e)
	{
		HandlerContext *new_ctx = new HandlerContext;
		new_ctx->reap_in_handler = false;
		new_ctx->event = e;
		new_ctx->handler_func = hdl->event->handlers[e->topic];
		create_handler_sync(&new_ctx->sync);
		hdl->event->current_handler = new_ctx;
		
		msa::thread::Attributes *attr = new msa::thread::Attributes;
		msa::thread::attr_init(attr);
		msa::thread::attr_set_detach(attr, true);
		
		new_ctx->running = true;
		int status = msa::thread::create(&new_ctx->thread, attr, event_start, hdl, "handler");
		if (status != 0)
		{
			msa::log::error(hdl, "Failed to start event handler thread; thread::create() returned " + std::to_string(status));
			new_ctx->running = false;
		}
		
		msa::thread::attr_destroy(attr);
		delete attr;
	}

	static void edt_dispatch_event(msa::Handle hdl, const Event *e)
	{
		if (hdl->event->current_handler != NULL)
		{
			edt_interrupt_handler(hdl);
		}
		// start the thread (if we have a handler)
		if (hdl->event->handlers[e->topic] != NULL)
		{
			edt_spawn_handler(hdl, e);
		}
		else
		{
			delete e;
		}
	}

	static void dispose_handler_context(HandlerContext *ctx, bool wait)
	{
		if (ctx->running)
		{
			if (handler_suspended(ctx->sync))
			{
				resume_handler(ctx->sync);
			}
			if (wait)
			{
				// wait until current event runs through
				while (ctx->running)
				{
					msa::util::sleep_milli(10);
				}
			}
			else
			{
				ctx->reap_in_handler = true;
			}
		}
		if (!ctx->reap_in_handler)
		{
			// delete event
			dispose(ctx->event);
			// delete sync handler
			dispose_handler_sync(ctx->sync);
			delete ctx;
		}
	}

	static void *event_start(void *args)
	{
		msa::Handle hdl = (msa::Handle) args;
		HandlerContext *ctx = hdl->event->current_handler;
		ctx->handler_func(hdl, ctx->event, ctx->sync);
		if (ctx->reap_in_handler)
		{
			dispose(ctx->event);
			dispose_handler_sync(ctx->sync);
			delete ctx;
		}
		else
		{
			ctx->running = false;
		}
		return NULL;
	}

	static void push_event(msa::Handle msa, const Event *e)
	{
		msa::thread::mutex_lock(&msa->event->queue_mutex);
		msa->event->queue.push(e);
		msa::thread::mutex_unlock(&msa->event->queue_mutex);
	}

	static void cmd_timer(msa::Handle hdl, const msa::cmd::ParamList &params, HandlerSync *const UNUSED(sync))
	{
		bool recurring = params.has_option('r');
		if (params.arg_count() < 2)
		{
			msa::agent::say(hdl, "You gotta give me a time and a command to execute, $USER_TITLE.");
			return;
		}
		int period = 0;
		try
		{
			period = std::stoi(params[0]);
		}
		catch (std::exception &e)
		{
			msa::agent::say(hdl, "Sorry, $USER_TITLE, but '" + params[0] + "' isn't a number of milliseconds.");
			return;
		}
		auto ms = std::chrono::milliseconds(period);
		std::string cmd_str = "";
		for (size_t i = 1; i < params.arg_count(); i++)
		{
			cmd_str += params[i];
			if (i + 1 < params.arg_count())
			{
				cmd_str += " ";
			}
		}
		std::string plural = ms.count() != 1 ? "s" : "";
		std::string type = recurring ? "every" : "in";
		int16_t id = -1;
		if (recurring)
		{
			id = add_timer(hdl, ms, Topic::TEXT_INPUT, wrap(cmd_str));
		}
		else
		{
			id = delay(hdl, ms, Topic::TEXT_INPUT, wrap(cmd_str));
		}
		if (id == -1)
		{
			msa::agent::say(hdl, "Oh no! I'm sorry, $USER_TITLE, that didn't work quite right!");
		}
		else
		{
			msa::agent::say(hdl, "Okay, $USER_TITLE, I will do that " + type + " " + std::to_string(ms.count()) + " millisecond" + plural + "!");
			msa::agent::say(hdl, "The timer ID is " + std::to_string(id) + ".");
		}
	}	

	static void cmd_deltimer(msa::Handle hdl, const msa::cmd::ParamList &params, HandlerSync *const UNUSED(sync))
	{
		if (params.arg_count() < 1)
		{
			msa::agent::say(hdl, "Ahh... $USER_TITLE, I need to know which timer I should delete.");
			return;
		}
		int16_t id = 0;
		try
		{
			id = std::stoi(params[0]);
		}
		catch (std::exception &e)
		{
			msa::agent::say(hdl, "Sorry, $USER_TITLE, but '" + params[0] + "' isn't an integer.");
			return;
		}
		remove_timer(hdl, id);
		msa::agent::say(hdl, "Okay! I stopped timer " + std::to_string(id) + " for you, $USER_TITLE.");
	}

} }
