#include "log.hpp"
#include "string.hpp"

#include <ofstream>
#include <vector>
#include <map>
#include <string>
#include <stdexcept>

#include <ctime>
#include <cstdio>

namespace msa { namespace log {

	static std::map<std::string, Level> LEVEL_NAMES;
	static std::map<std::string, Format> FORMAT_NAMES;
	static std::map<std::string, StreamType> STREAM_TYPE_NAMES;
	static std::string XML_FORMAT_STRING = "<entry><time>%1$s</time><level>%2$s</level><message>%3$s</message></entry>";

	typedef int (*CloseHandler)(std::ostream *raw_stream);

	typedef struct message_context_type
	{
		const std::string *msg;
		Level level;
		time_t time;
	} MessageContext;

	typedef struct log_stream_type
	{
		std::ostream *out;
		std::string output_format_string;
		StreamType type;
		CloseHandler close_handler;
		Level level;
		Format format;
	} LogStream;

	struct log_context_type
	{
		std::vector<LogStream *> streams;
		Level level;
	}

	static int create_log_context(LogContext **ctx);
	static int dispose_log_context(LogContext *ctx);
	static int create_log_stream(LogStream **stream);
	static int dispose_log_stream(LogStream *stream);
	static int close_ofstream(std::ostream *raw_stream);
	static void check_and_write(msa::Handle hdl, const std::string &msg, Level level);
	static void write(LogStream *stream, const MessageContext &ctx);
	static void write_xml(LogStream *stream, const MessageContext &ctx);
	static void write_text(LogStream *stream, const MessageContext &ctx);
	static const char *level_to_str(Level lev);

	extern int init(msa::Handle hdl, const msa::config::Section &config)
	{
		// init static resources
		if (LEVEL_NAMES.empty())
		{
			LEVEL_NAMES["TRACE"] = Level::TRACE;
			LEVEL_NAMES["DEBUG"] = Level::DEBUG;
			LEVEL_NAMES["INFO"] = Level::INFO;
			LEVEL_NAMES["WARNING"] = Level::WARNING;
			LEVEL_NAMES["ERROR"] = Level::ERROR;
		}
		if (FORMAT_NAMES.empty())
		{
			FORMAT_NAMES["TEXT"] = Format::TEXT;
			FORMAT_NAMES["XML"] = Format::XML;
		}
		if (STREAM_TYPE_NAMES.empty())
		{
			STREAM_TYPE_NAMES["FILE"] = StreamType::FILE;
		}

		create_log_context(hdl->log);

		// first check config for global level
		std::string gl_level_str = config.get_or("GLOBAL_LEVEL", "INFO");
		msa::util::to_upper(gl_level_str);
		if (LEVEL_NAMES.find(gl_level_str) == LEVEL_NAMES.end())
		{
			throw std::invalid_argument("'" + gl_level_str + "' is not a valid log level");
		}
		hdl->log->level = LEVEL_NAMES[gl_level_str];
		
		// now check config for individual streams
		if (config.has("TYPE") && config.has("LOCATION"))
		{
			const std::vector<std::string> types = config.get_all("TYPE");
			const std::vector<std::string> locs = config.get_all("LOCATION");
			const std::vector<std::string> levs = config.has("LEVEL") ? config.get_all("LEVEL") : std::vector<std::string>();
			const std::vector<std::string> fmts = config.has("FORMAT") ? config.get_all("FORMAT") : std::vector<std::string>();
			const std::vector<std::string> outputs = config.has("OUTPUT") ? config.get_all("OUTPUT") : std::vector<std::string>();

			for (size_t i = 0; i < types.size() && i < locs.size())
			{
				std::string type_str = types[i];
				std::string location = locs[i];
				std::string lev_str = levs.size() > i ? levs[i] : "info";
				std::string fmt_str = fmts.size() > i ? fmts[i] : "xml";
				msa::util::to_upper(type_str);
				msa::util::to_upper(lev_str);
				msa::util::to_upper(fmt_str);
				if (FORMAT_NAMES.find(fmt_str) == FORMAT_NAMES.end())
				{
					throw std::invalid_argument("'" + fmt_str + "' is not a valid log format");
				}
				if (LEVEL_NAMES.find(lev_str) == LEVEL_NAMES.end())
				{
					throw std::invalid_argument("'" + lev_str + "' is not a valid log level");
				}
				if (STREAM_TYPE_NAMES.find(type_str) == STREAM_TYPE_NAMES.end())
				{
					throw std::invalid_argument("'" + type_str + "' is not a valid log stream type");
				}
				StreamType type = STREAM_TYPE_NAMES[type_str];
				Level lev = LEVEL_NAMES[lev_str];
				Format fmt = FORMAT_NAMES[fmt_str];
				std::string output;
				if (fmt == Format::TEXT)
				{
					if (outputs.size() >= i) {
						throw std::invalid_argument("TEXT log format requires OUTPUT parameter");
					}
					output = outputs.at(i);
				}
				else if (fmt == Format::XML)
				{
					output = XML_OUTPUT_STRING;
				}
				
				stream_id id = create_stream(hdl, type, location, fmt, output);
				set_stream_level(hdl, id, lev);
			}
		}
	}

	extern int quit(msa::Handle hdl)
	{
		dispose_log_context(hdl->log);
		return 0;
	}

	extern stream_id create_stream(msa::Handle hdl, StreamType type, const std::string &location, Format fmt, const std::string &output_format_string)
	{
		LogStream *s;
		if (create_log_stream(&s) != 0)
		{
			throw std::logic_error("could not create log stream");
		}
		s->type = type;
		s->format = fmt;
		s->output_format_string = output_format_string;
		// now actually open the stream
		if (s->type == StreamType::FILE)
		{
			std::ofstream *file = new std::ofstream;
			file->exceptions(std::ofstream::failbit | std::ofstream::badbit);
			file->open(location, std::ofstream::out | std::ofstream::app);
			if (!file->is_open())
			{
				throw std::logic_error("could not open log stream file");
			}
			s->out = file;
			s->close_handler = close_ofstream;
		}
		else
		{
			dispose_log_stream(s);
			throw std::invalid_argument("unknown log stream type: " + std::to_string(s->type);
		}

		hdl->log->streams.push_back(*s);
		return (stream_id) (hdl->log->streams.size() - 1);
	}

	extern void set_level(msa::Handle hdl, Level level)
	{
		hdl->log->level = level;
	}

	extern Level get_level(msa::Handle hdl)
	{
		return hdl->log->level;
	}

	extern void set_stream_level(msa::Handle hdl, stream_id id, Level level)
	{
		hdl->log->streams.at(id)->level = level;
	}
	
	extern Level get_stream_level(msa::Handle hdl, stream_id id)
	{
		return hdl->log->streams.at(id)->level;
	}

	extern void trace(msa::Handle hdl, const std::string &msg)
	{
		check_and_write(hdl, msg, Level::TRACE);
	}

	extern void trace(msa::Handle hdl, const char *msg)
	{
		std::string msg_str = std::string(msg);
		trace(hdl, msg_str);
	}

	extern void debug(msa::Handle hdl, const std::string &msg)
	{
		check_and_write(hdl, msg, Level::DEBUG);
	}

	extern void debug(msa::Handle hdl, const char *msg)
	{
		std::string msg_str = std::string(msg);
		debug(hdl, msg_str);
	}

	extern void info(msa::Handle hdl, const std::string &msg)
	{
		check_and_write(hdl, msg, Level::INFO);
	}

	extern void info(msa::Handle hdl, const char *msg)
	{
		std::string msg_str = std::string(msg);
		info(hdl, msg_str);
	}

	extern void warn(msa::Handle hdl, const std::string &msg)
	{
		check_and_write(hdl, msg, Level::WARN);
	}

	extern void warn(msa::Handle hdl, const char *msg)
	{
		std::string msg_str = std::string(msg);
		warn(hdl, msg_str);
	}

	extern void error(msa::Handle hdl, const std::string &msg)
	{
		check_and_write(hdl, msg, Level::ERROR);
	}

	extern void error(msa::Handle hdl, const char *msg)
	{
		std::string msg_str = std::string(msg);
		error(hdl, msg_str);
	}

	static void check_and_write(msa::Handle hdl, const std::string &msg, Level level)
	{
		LogContext *ctx = hdl->log;
		// check if we should ignore the message from the global level
		if (level < ctx->level) {
			return;
		}

		// create the context
		MessageContext msg_ctx;
		msg_ctx.msg = &msg;
		msg_ctx.level = level;
		time(&msg_ctx.time);
		
		for (size_t i = 0; i < ctx->streams.size(); i++)
		{
			LogStream *stream = ctx->streams.get(i);
			if (level >= stream->level)
			{
				write(stream, msg_ctx);
			}
		}
	}

	static void write(LogStream *stream, const MessageContext &ctx)
	{
		if (stream->format == Format::XML)
		{
			write_xml(stream, ctx);
		}
		else if (stream->format == Format::TEXT)
		{
			write_text(stream, ctx);
		}
		else
		{
			throw std::logic_error("bad log stream format");
		}
	}

	static void write_xml(LogStream *stream, const MessageContext &ctx)
	{
		static char buffer[512];
		struct tm *time_info = gmtime(&ctx.time);
		const char *timestr = asctime(time_info);
		const char *lev_str = level_to_str(ctx.level);
		sprintf(buffer, stream->output_string_format, timestr, lev_str, ctx.msg.c_str());
		stream->out << buffer << std::endl;
	}

	static void write_text(LogStream *stream, const MessageContext &ctx)
	{
		static char buffer[512];
		struct tm *time_info = localtime(&ctx.time);
		const char *timestr = asctime(time_info);
		const char *lev_str = level_to_str(ctx.level);
		sprintf(buffer, stream->output_string_format, timestr, lev_str, ctx.msg.c_str());
		stream->out << buffer << std::endl;
	}

	static const char *level_to_str(Level lev)
	{
		if (lev == Level::TRACE)
		{
			return "TRACE";
		}
		else if (lev == Level::DEBUG)
		{
			return "DEBUG";
		}
		else if (lev == Level::INFO)
		{
			return "INFO";
		}
		else if (lev == Level::WARN)
		{
			return "WARN";
		}
		else if (lev == Level::ERROR)
		{
			return "ERROR";
		}
		else
		{
			return "UNKNOWN";
		}
	}

	static int create_log_context(LogContext **ctx)
	{
		LogContext *log = new LogContext;
		*ctx = log;
		return 0;
	}

	static int dispose_log_context(LogContext *ctx)
	{
		while (!ctx->streams.empty())
		{
			LogStream *stream = *(ctx->streams.begin());
			if (dispose_log_stream(stream) != 0)
			{
				return 1;
			}
			ctx->streams.erase(ctx->streams.begin());
		}
		delete ctx;
		return 0;
	}

	static int create_log_stream(LogStream **stream)
	{
		LogStream *st = new LogStream;
		st->out = NULL;
		st->level = Level::TRACE;
		st->format = Format::TEXT;
		st->close_handler = NULL;
		*stream = st;
		return 0;
	}

	static int dispose_log_stream(LogStream *stream)
	{
		if (stream->out != NULL)
		{
			int stat = stream->close_handler(stream->out);
			if (stat != 0)
			{
				return stat;
			}
			delete stream->out;
		}
		delete stream;
		return 0;
	}

	static int close_ofstream(std::ostream *raw_stream)
	{
		std::ofstream *file = static_cast<std::ofstream *>(raw_stream);
		try {
			if (file->is_open())
			{
				file->close();
			}
		}
		catch (...)
		{
			return 1;
		}
		return 0;
	}

} }