#include "msa.hpp"
#include "edt.hpp"
#include "input.hpp"

namespace msa {	

	extern int init(Handle *msa)
	{		
		environment_type *hdl = new environment_type;
		hdl->status = Status::CREATED;
		hdl->event = NULL;
		hdl->input = NULL;
		int ret = msa::event::init(hdl);
		if (ret != 0)
		{
			quit(hdl);
			dispose(hdl);
			return ret;
		}
		ret = msa::io::init(hdl);
		if (ret != 0)
		{
			quit(hdl);
			dispose(hdl);
			return ret;
		}
		*msa = hdl;
		return 0;
	}

	extern int quit(Handle *msa)
	{
		if (msa->event != NULL)
		{
			msa::event::quit(msa);
			msa->event = NULL;
		}
		if (msa->input != NULL)
		{
			msa::io::quit(msa);
			msa->input = NULL;
		}
		return msa;
	}

	extern int dispose(msa::Handle msa)
	{
		// make sure our modules have been properly quit before deleting the pointer
		if (msa->event != NULL)
		{
			return 1;
		}
		if (msa->input != NULL)
		{
			return 2;
		}
		delete msa;
		return 0;
	}

}
