#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

namespace msa {

	namespace io {

		typedef struct input_context_type InputContext;

	}

	namespace event {
		
		typedef struct event_dispatch_context_type EventDispatchContext;
		
	}

	typedef enum status_type { CREATED, RUNNING, STOP_REQUESTED, STOPPED } Status;

	struct environment_type
	{
		Status status;
		msa::event::EventDispatchContext *event;
		msa::io::InputContext *input;
	};

	typedef struct environment_type* Handle;

}

#endif
