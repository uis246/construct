// Matrix Construct
//
// Copyright (C) Matrix Construct Developers, Authors & Contributors
// Copyright (C) 2016-2018 Jason Volk <jason@zemos.net>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice is present in all copies. The
// full license for this software is available in the LICENSE file.

using namespace ircd;

mapi::header
IRCD_MODULE
{
	"Federation :General Library and Utils"
};

namespace ircd::m::feds
{
	template<class T>
	struct request;

	template<class T>
	static bool
	handler(const opts &, const closure &, std::list<request<T>> &);

	template<class T>
	static std::list<request<T>>
	creator(const opts &, const std::function<T (request<T> &, const string_view &origin)> &);
}

template<class T>
struct ircd::m::feds::request
:T
{
	char origin[256];
	char buf[8_KiB];

	request(const std::function<T (request &)> &closure)
	:T(closure(*this))
	{}

	request(request &&) = delete;
	request(const request &) = delete;
};

///////////////////////////////////////////////////////////////////////////////
//
// m/feds.h
//

bool
IRCD_MODULE_EXPORT
ircd::m::feds::version(const opts &opts,
                       const closure &closure)
{
	static const auto make_request{[]
	(auto &request, const auto &origin)
	{
		m::v1::version::opts opts;
		opts.dynamic = false;
		opts.remote = string_view
		{
			request.origin, strlcpy{request.origin, origin}
		};

		return m::v1::version
		{
			request.buf, std::move(opts)
		};
	}};

	auto requests
	{
		creator<m::v1::version>(opts, make_request)
	};

	return handler(opts, closure, requests);
}

bool
IRCD_MODULE_EXPORT
ircd::m::feds::state(const opts &opts,
                     const closure &closure)
{
	const auto make_request{[&opts]
	(auto &request, const auto &origin)
	{
		m::v1::state::opts v1opts;
		v1opts.dynamic = true;
		v1opts.ids_only = opts.ids;
		v1opts.event_id = opts.event_id;
		v1opts.remote = string_view
		{
			strlcpy{request.origin, origin}
		};

		return m::v1::state
		{
			opts.room_id, request.buf, std::move(v1opts)
		};
	}};

	auto requests
	{
		creator<m::v1::state>(opts, make_request)
	};

	return handler(opts, closure, requests);
}

bool
IRCD_MODULE_EXPORT
ircd::m::feds::event(const opts &opts,
                     const closure &closure)
{
	const auto make_request{[&opts]
	(auto &request, const auto &origin)
	{
		m::v1::event::opts v1opts;
		v1opts.dynamic = true;
		v1opts.remote = string_view
		{
			request.origin, strlcpy{request.origin, origin}
		};

		return m::v1::event
		{
			opts.event_id, request.buf, std::move(v1opts)
		};
	}};

	auto requests
	{
		creator<m::v1::event>(opts, make_request)
	};

	return handler(opts, closure, requests);
}

bool
IRCD_MODULE_EXPORT
ircd::m::feds::head(const opts &opts,
                    const closure &closure)
{
	const auto make_request{[&opts]
	(auto &request, const auto &origin)
	{
		m::v1::make_join::opts v1opts;
		v1opts.remote = string_view
		{
			strlcpy{request.origin, origin}
		};

		return m::v1::make_join
		{
			opts.room_id, opts.user_id, request.buf, std::move(v1opts)
		};
	}};

	auto requests
	{
		creator<m::v1::make_join>(opts, make_request)
	};

	return handler(opts, closure, requests);
}

///////////////////////////////////////////////////////////////////////////////
//
// (internal)
//

template<class T>
std::list<m::feds::request<T>>
ircd::m::feds::creator(const opts &opts,
                       const std::function<T (request<T> &, const string_view &origin)> &closure)
{
	assert(opts.room_id);
	const m::room::origins origins
	{
		opts.room_id
	};

	std::list<m::feds::request<T>> ret;
	origins.for_each([&ret, &closure]
	(const string_view &origin)
	{
		if(!server::errmsg(origin)) try
		{
			ret.emplace_back([&closure, &origin]
			(auto &request)
			{
				return closure(request, origin);
			});
		}
		catch(const std::exception &)
		{
			return;
		}
	});

	return ret;
}

template<class T>
bool
ircd::m::feds::handler(const opts &opts,
                       const closure &closure,
                       std::list<request<T>> &reqs)
{
	const auto when
	{
		now<steady_point>() + opts.timeout
	};

	while(!reqs.empty())
	{
		auto next
		{
			ctx::when_any(begin(reqs), end(reqs))
		};

		if(!next.wait_until(when, std::nothrow))
			break;

		const auto it
		{
			next.get()
		};

		auto &req{*it}; try
		{
			const auto code{req.get()};
			const json::array &array{req.in.content};
			const json::object &object{req.in.content};
			if(!closure({req.origin, {}, object, array}))
				return false;
		}
		catch(const std::exception &)
		{
			const ctx::exception_handler eptr;
			if(!closure({req.origin, eptr}))
				return false;
		}

		reqs.erase(it);
	}

	return true;
}

//
// legacy
//

extern "C" void
feds__event(const m::event::id &event_id, std::ostream &out)
{
	const m::room::id::buf room_id{[&event_id]
	{
		const m::event::fetch event
		{
			event_id
		};

		return m::room::id::buf{at<"room_id"_>(event)};
	}()};

	struct req
	:m::v1::event
	{
		char origin[256];
		char buf[96_KiB];

		req(const m::event::id &event_id, const string_view &origin)
		:m::v1::event{[&]
		{
			m::v1::event::opts opts;
			opts.dynamic = false;
			opts.remote = string_view{this->origin, strlcpy(this->origin, origin)};
			return m::v1::event{event_id, mutable_buffer{buf}, std::move(opts)};
		}()}
		{}
	};

	std::list<req> reqs;
	const m::room::origins origins{room_id};
	origins.for_each([&out, &event_id, &reqs]
	(const string_view &origin)
	{
		const auto emsg
		{
			ircd::server::errmsg(origin)
		};

		if(emsg)
		{
			out << "! " << origin << " " << emsg << std::endl;
			return;
		}

		try
		{
			reqs.emplace_back(event_id, origin);
		}
		catch(const std::exception &e)
		{
			out << "! " << origin << " " << e.what() << std::endl;
			return;
		}
	});

	auto all
	{
		ctx::when_all(begin(reqs), end(reqs))
	};

	all.wait(30s, std::nothrow);

	for(auto &req : reqs) try
	{
		if(req.wait(1ms, std::nothrow))
		{
			const auto code{req.get()};
			out << "+ " << req.origin << " " << http::status(code) << std::endl;
		}
		else cancel(req);
	}
	catch(const std::exception &e)
	{
		out << "- " << req.origin << " " << e.what() << std::endl;
	}
}

extern "C" void
feds__backfill(const m::room::id &room_id,
               const m::event::id &event_id,
               const size_t &limit,
               std::ostream &out)
{
	struct req
	:m::v1::backfill
	{
		char origin[256];
		char buf[16_KiB];

		req(const m::room::id &room_id, const m::event::id &event_id, const string_view &origin, const size_t &limit)
		:m::v1::backfill{[&]
		{
			m::v1::backfill::opts opts;
			opts.remote = string_view{this->origin, strlcpy(this->origin, origin)};
			opts.event_id = event_id;
			opts.limit = limit;
			opts.dynamic = true;
			return m::v1::backfill{room_id, mutable_buffer{buf}, std::move(opts)};
		}()}
		{}
	};

	std::list<req> reqs;
	const m::room::origins origins{room_id};
	origins.for_each([&out, &room_id, &event_id, &limit, &reqs]
	(const string_view &origin)
	{
		const auto emsg
		{
			ircd::server::errmsg(origin)
		};

		if(!emsg) try
		{
			reqs.emplace_back(room_id, event_id, origin, limit);
		}
		catch(const std::exception &e)
		{
			return;
		}
	});

	auto all
	{
		ctx::when_all(begin(reqs), end(reqs))
	};

	all.wait(30s, std::nothrow);

	std::map<string_view, std::set<const req *>> grid;

	for(auto &req : reqs) try
	{
		if(req.wait(1ms, std::nothrow))
		{
			const auto code{req.get()};
			const json::object &response{req};
			const json::array pdus
			{
				response.at("pdus")
			};

			for(const json::object &pdu : pdus)
			{
				const auto &event_id
				{
					unquote(pdu.at("event_id"))
				};

				grid[event_id].emplace(&req);
			}
		}
		else cancel(req);
	}
	catch(const std::exception &e)
	{
		out << "- " << req.origin << " " << e.what() << std::endl;
	}

	size_t i(0);
	for(const auto &p : grid)
		out << i++ << " " << p.first << std::endl;

	for(size_t j(0); j < i; ++j)
		out << "| " << std::left << std::setw(2) << j;
	out << "|" << std::endl;

	for(const auto &req : reqs)
	{
		for(const auto &p : grid)
			out << "| " << (p.second.count(&req)? '+' : ' ') << " ";
		out << "| " << req.origin << std::endl;
	}
}

namespace ircd::m::feds::v1
{
	struct perspective;
}

struct ircd::m::feds::v1::perspective
:m::v1::key::query
{
	using closure = std::function<bool (const string_view &, std::exception_ptr, const json::array &)>;

	char origin[256];
	char buf[24_KiB];

	perspective(const string_view &origin,
	            const m::v1::key::server_key &server_key)
	:m::v1::key::query{[&]
	{
		m::v1::key::opts opts;
		opts.dynamic = false;
		opts.remote = string_view{this->origin, strlcpy(this->origin, origin)};
		return m::v1::key::query
		{
			{&server_key, 1}, mutable_buffer{buf}, std::move(opts)
		};
	}()}
	{}

	perspective(perspective &&) = delete;
	perspective(const perspective &) = delete;
};

std::list<m::feds::v1::perspective>
feds__perspective(const m::room::id &room_id,
                  const m::v1::key::server_key &server_key)
{
	std::list<m::feds::v1::perspective> reqs;
	m::room::origins{room_id}.for_each([&reqs, &server_key]
	(const string_view &origin)
	{
		const auto emsg
		{
			ircd::server::errmsg(origin)
		};

		if(!emsg) try
		{
			reqs.emplace_back(origin, server_key);
		}
		catch(const std::exception &)
		{
			return;
		}
	});

	return std::move(reqs);
}

extern "C" void
feds__perspective(const m::room::id &room_id,
                  const m::v1::key::server_key &server_key, // pair<server_name, key_id>
                  const milliseconds &timeout,
                  const m::feds::v1::perspective::closure &closure)
{
	auto reqs
	{
		feds__perspective(room_id, server_key)
	};

	auto when
	{
		now<steady_point>() + timeout
	};

	while(!reqs.empty())
	{
		auto next
		{
			ctx::when_any(begin(reqs), end(reqs))
		};

		if(!next.wait_until(when, std::nothrow))
			break;

		const auto it
		{
			next.get()
		};

		auto &req{*it}; try
		{
			const auto code{req.get()};
			const json::array &response{req};
			if(!closure(req.origin, {}, response))
				break;
		}
		catch(const std::exception &)
		{
			if(!closure(req.origin, std::current_exception(), {}))
				break;
		}

		reqs.erase(it);
	}
}
