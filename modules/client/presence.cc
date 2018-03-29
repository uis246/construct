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
	"Client 11.6 :Presence"
};

ircd::resource
presence_resource
{
	"/_matrix/client/r0/presence/", resource::opts
	{
		"(11.6.2) Presence",
		resource::DIRECTORY,
	}
};

//
// put
//

extern "C" m::event::id::buf
commit__m_presence(const m::presence &content);

static resource::response
put__presence_status(client &client,
                     const resource::request &request,
                     const m::user::id &user_id);

static resource::response
put__presence(client &client,
              const resource::request &request);

resource::method
method_put
{
	presence_resource, "PUT", put__presence,
	{
		method_put.REQUIRES_AUTH
	}
};

resource::response
put__presence(client &client,
              const resource::request &request)
{
	if(request.parv.size() < 1)
		throw m::NEED_MORE_PARAMS
		{
			"user_id required"
		};

	m::user::id::buf user_id
	{
		url::decode(request.parv[0], user_id)
	};

	if(user_id != request.user_id)
		throw m::FORBIDDEN
		{
			"You cannot set the presence of '%s' when you are '%s'",
			user_id,
			request.user_id
		};

	if(request.parv.size() < 2)
		throw m::NEED_MORE_PARAMS
		{
			"command required"
		};

	const auto &cmd
	{
		request.parv[1]
	};

	if(cmd == "status")
		return put__presence_status(client, request, user_id);

	throw m::NOT_FOUND
	{
		"Presence command not found"
	};
}

resource::response
put__presence_status(client &client,
                     const resource::request &request,
                     const m::user::id &user_id)
{
	const string_view &presence
	{
		unquote(request.at("presence"))
	};

	if(!m::presence::valid_state(presence))
		throw m::UNSUPPORTED
		{
			"That presence state is not supported"
		};

	const string_view &status_msg
	{
		trunc(unquote(request["status_msg"]), 390)
	};

	const m::user user{request.user_id};
	m::presence::set(user, presence, status_msg);
	return resource::response
	{
		client, http::OK
	};
}

extern "C" m::event::id::buf
commit__m_presence(const m::presence &content)
{
	const m::user user
	{
		at<"user_id"_>(content)
	};

	//TODO: ABA
	if(!exists(user))
		create(user.user_id);

	const m::user::room user_room
	{
		user
	};

	//TODO: ABA
	return send(user_room, user.user_id, "m.presence", json::strung{content});
}

//
// get
//

extern "C" json::object
m_presence_get(const m::user &user,
              const mutable_buffer &buffer);

static resource::response
get__presence_status(client &client,
                     const resource::request &request,
                     const m::user::id &user_id)
{
	const m::user::room user_room
	{
		user_id
	};

	user_room.get("m.presence", [&client]
	(const m::event &event)
	{
		const auto &content
		{
			at<"content"_>(event)
		};

		json::iov response;
		const json::iov::push _presence
		{
			response, { "presence", content.at("presence") }
		};

		const json::iov::set_if _status_msg
		{
			response, content.has("status_msg"),
			{
				"status_msg", content.at("status_msg")
			}
		};

		//TODO: better last_active_ago
		const auto last_active_ago
		{
			ircd::now<milliseconds>() - milliseconds(at<"origin_server_ts"_>(event))
		};

		const json::iov::push _last_active_ago
		{
			response, { "last_active_ago", last_active_ago.count() }
		};

		//TODO: better currently_active
		const bool currently_active
		{
			content.at("presence") == "online"
		};

		const json::iov::push _currently_active
		{
			response, { "currently_active", currently_active }
		};

		resource::response
		{
			client, response
		};
	});

	return {}; // responded from closure
}

static resource::response
get__presence_list(client &client,
                   const resource::request &request)
{
	if(request.parv.size() < 2)
		throw m::NEED_MORE_PARAMS
		{
			"user_id required"
		};

	m::user::id::buf user_id
	{
		url::decode(request.parv[1], user_id)
	};

	const m::user::room user_room
	{
		user_id
	};

	//TODO: reuse composition from /status
	std::vector<json::value> list;
	return resource::response
	{
		client, json::value
		{
			list.data(), list.size()
		}
	};
}

resource::response
get__presence(client &client,
              const resource::request &request)
{
	if(request.parv.size() < 1)
		throw m::NEED_MORE_PARAMS
		{
			"user_id or command required"
		};

	if(request.parv[0] == "list")
		return get__presence_list(client, request);

	m::user::id::buf user_id
	{
		url::decode(request.parv[0], user_id)
	};

	if(request.parv.size() < 2)
		throw m::NEED_MORE_PARAMS
		{
			"command required"
		};

	const auto &cmd
	{
		request.parv[1]
	};

	if(cmd == "status")
		return get__presence_status(client, request, user_id);

	throw m::NOT_FOUND
	{
		"Presence command not found"
	};
}

resource::method
method_get
{
	presence_resource, "GET", get__presence
};

json::object
m_presence_get(const m::user &user,
              const mutable_buffer &buffer)
{
	const m::user::room user_room
	{
		user
	};

	json::object ret;
	user_room.get(std::nothrow, "m.presence", [&ret, &buffer]
	(const m::event &event)
	{
		const string_view &content
		{
			at<"content"_>(event)
		};

		ret = { data(buffer), copy(buffer, content) };
	});

	return ret;
}

//
// POST ?
//

static resource::response
post__presence_list(client &client,
                    const resource::request &request)
{
	if(request.parv.size() < 2)
		throw m::NEED_MORE_PARAMS
		{
			"user_id required"
		};

	m::user::id::buf user_id
	{
		url::decode(request.parv[1], user_id)
	};

	const m::user::room user_room
	{
		user_id
	};

	return resource::response
	{
		client, http::OK
	};
}

resource::response
post__presence(client &client,
               const resource::request &request)
{
	if(request.parv.size() < 1)
		throw m::NEED_MORE_PARAMS
		{
			"command required"
		};

	if(request.parv[0] == "list")
		return get__presence_list(client, request);

	throw m::NOT_FOUND
	{
		"Presence command not found"
	};
}

resource::method
method_post
{
	presence_resource, "POST", post__presence,
	{
		method_post.REQUIRES_AUTH
	}
};
