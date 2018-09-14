// Matrix Construct
//
// Copyright (C) Matrix Construct Developers, Authors & Contributors
// Copyright (C) 2016-2018 Jason Volk <jason@zemos.net>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice is present in all copies. The
// full license for this software is available in the LICENSE file.

using namespace ircd::m;
using namespace ircd;

mapi::header
IRCD_MODULE
{
	"Matrix user library; modular components."
};

extern "C" bool
highlighted_event(const event &event,
                  const user &user)
{
	if(json::get<"type"_>(event) != "m.room.message")
		return false;

	const json::object &content
	{
		json::get<"content"_>(event)
	};

	const string_view &body
	{
		content.get("body")
	};

	return has(body, user.user_id);
}

extern "C" size_t
highlighted_count__between(const user &user,
                           const room &room,
                           const event::idx &a,
                           const event::idx &b)
{
	static const event::fetch::opts fopts
	{
		event::keys::include
		{
			"type", "content",
		}
	};

	room::messages it
	{
		room, &fopts
	};

	assert(a <= b);
	it.seek_idx(a);

	if(!it && !exists(room))
		throw NOT_FOUND
		{
			"Cannot find room '%s' to count highlights for '%s'",
			string_view{room.room_id},
			string_view{user.user_id}
		};
	else if(!it)
		throw NOT_FOUND
		{
			"Event @ idx:%lu or idx:%lu not found in '%s' to count highlights for '%s'",
			a,
			b,
			string_view{room.room_id},
			string_view{user.user_id},
		};

	size_t ret{0};
	for(++it; it && it.event_idx() <= b; ++it)
	{
		const event &event{*it};
		ret += highlighted_event(event, user);
	}

	return ret;
}
