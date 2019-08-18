// Matrix Construct
//
// Copyright (C) Matrix Construct Developers, Authors & Contributors
// Copyright (C) 2016-2019 Jason Volk <jason@zemos.net>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice is present in all copies. The
// full license for this software is available in the LICENSE file.

namespace ircd::m
{
	static void check_room_auth_rule_9(const m::event &, room::auth::hookdata &);
	static void check_room_auth_rule_8(const m::event &, room::auth::hookdata &);
	static void check_room_auth_rule_6(const m::event &, room::auth::hookdata &);
	static void check_room_auth_rule_3(const m::event &, room::auth::hookdata &);
	static void check_room_auth_rule_2(const m::event &, room::auth::hookdata &);

	extern hook::site<room::auth::hookdata &> room_auth_hook;
}

ircd::mapi::header
IRCD_MODULE
{
	"Matrix room event authentication support."
};

decltype(ircd::m::room_auth_hook)
ircd::m::room_auth_hook
{
	{ "name",          "room.auth" },
	{ "exceptions",    true        },
};

//
// generate
//

ircd::json::array
IRCD_MODULE_EXPORT
ircd::m::room::auth::generate(const mutable_buffer &buf,
                              const m::room &room,
                              const m::event &event)
{
	json::stack out{buf};
	json::stack::checkpoint cp{out};
	{
		json::stack::array array{out};
		if(!generate(array, room, event))
			cp.decommit();
	}

	return json::array{out.completed()};
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::generate(json::stack::array &out,
                              const m::room &room,
                              const m::event &event)
{
	const m::event::id::closure &v1_ref{[&out]
	(const auto &event_id)
	{
		json::stack::array auth{out};
		auth.append(event_id);
		{
			json::stack::object nilly{auth};
			json::stack::member willy
			{
				nilly, "", ""
			};
		}
	}};

	const m::event::id::closure &v3_ref{[&out]
	(const auto &event_id)
	{
		out.append(event_id);
	}};

	char versionbuf[64];
	const auto version
	{
		m::version(versionbuf, room, std::nothrow)
	};

	assert(version);
	const auto &fetch_append
	{
		version == "1" || version == "2"? v1_ref : v3_ref
	};

	const m::room::state state
	{
		room
	};

	const auto &type
	{
		json::get<"type"_>(event)
	};

	if(!type)
		return false;

	if(type == "m.room.create")
		return false;

	state.get(std::nothrow, "m.room.create", "", fetch_append);
	state.get(std::nothrow, "m.room.power_levels", "", fetch_append);

	if(type == "m.room.member")
		if(!m::membership(event) || m::membership(event) == "join" || m::membership(event) == "invite")
			state.get(std::nothrow, "m.room.join_rules", "", fetch_append);

	const string_view member_sender
	{
		defined(json::get<"sender"_>(event))?
			m::user::id{at<"sender"_>(event)}:
			m::user::id{}
	};

	if(member_sender)
		state.get(std::nothrow, "m.room.member", member_sender, fetch_append);

	m::user::id member_target;
	if(json::get<"sender"_>(event) && json::get<"state_key"_>(event))
		if(at<"sender"_>(event) != at<"state_key"_>(event))
			if(valid(m::id::USER, at<"state_key"_>(event)))
				member_target = at<"state_key"_>(event);

	if(member_target)
		state.get(std::nothrow, "m.room.member", member_target, fetch_append);

	return true;
}

//
// check
//

void
IRCD_MODULE_EXPORT
ircd::m::room::auth::check(const event &event)
{
	const auto &[pass, fail]
	{
		check(std::nothrow, event)
	};

	if(!pass)
	{
		assert(bool(fail));
		std::rethrow_exception(fail);
		__builtin_unreachable();
	}
}

ircd::m::room::auth::passfail
IRCD_MODULE_EXPORT
ircd::m::room::auth::check(std::nothrow_t,
                           const event &event)
{
	using json::at;

	const m::room room
	{
		at<"room_id"_>(event)
	};

	const m::event::prev refs{event};
	const auto count
	{
		refs.auth_events_count()
	};

	if(count > 4)
		log::dwarning
		{
			"Event %s has an unexpected %zu auth_events references",
			string_view{event.event_id},
			count,
		};

	// Vector of contingent event idxs from the event and the present state
	m::event::idx idxs[9]
	{
		count > 0? m::index(refs.auth_event(0)): 0UL,
		count > 1? m::index(refs.auth_event(1)): 0UL,
		count > 2? m::index(refs.auth_event(2)): 0UL,
		count > 3? m::index(refs.auth_event(3)): 0UL,

		room.get(std::nothrow, "m.room.create", ""),
		room.get(std::nothrow, "m.room.power_levels", ""),
		room.get(std::nothrow, "m.room.member", at<"sender"_>(event)),

		at<"type"_>(event) == "m.room.member" &&
		(membership(event) == "join" || membership(event) == "invite")?
			room.get(std::nothrow, "m.room.join_rules", ""): 0UL,

		at<"type"_>(event) == "m.room.member" &&
		at<"sender"_>(event) != at<"state_key"_>(event)?
			room.get(std::nothrow, "m.room.member", at<"state_key"_>(event)): 0UL,
	};

	m::event::fetch auth[9];
	for(size_t i(0); i < 9; ++i)
		if(idxs[i])
			seek(auth[i], idxs[i], std::nothrow);

	size_t i, j;
	const m::event *authv[4];
	for(i = 0, j = 0; i < 4 && j < 4; ++i)
		if(auth[i].valid)
			authv[j++] = &auth[i];

	hookdata data
	{
		event, {authv, j}
	};

	auto ret
	{
		check(std::nothrow, event, data)
	};

	if(!std::get<bool>(ret) || std::get<std::exception_ptr>(ret))
		return ret;

	for(i = 4, j = 0; i < 9 && j < 4; ++i)
		if(auth[i].valid)
			authv[j++] = &auth[i];

	data =
	{
		event, {authv, j}
	};

	ret =
	{
		check(std::nothrow, event, data)
	};

	return ret;
}

ircd::m::room::auth::passfail
IRCD_MODULE_EXPORT
ircd::m::room::auth::check(std::nothrow_t,
                           const event &event,
                           hookdata &data)
try
{
	// 1. If type is m.room.create:
	if(json::get<"type"_>(event) == "m.room.create")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// 2. Reject if event has auth_events that:
	check_room_auth_rule_2(event, data);

	// 3. If event does not have a m.room.create in its auth_events, reject.
	check_room_auth_rule_3(event, data);

	// 4. If type is m.room.aliases
	if(json::get<"type"_>(event) == "m.room.aliases")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// 5. If type is m.room.member
	if(json::get<"type"_>(event) == "m.room.member")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// 6. If the sender's current membership state is not join, reject.
	check_room_auth_rule_6(event, data);

	// 7. If type is m.room.third_party_invite:
	if(json::get<"type"_>(event) == "m.room.third_party_invite")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// 8. If the event type's required power level is greater than the
	// sender's power level, reject.
	check_room_auth_rule_8(event, data);

	// 9. If the event has a state_key that starts with an @ and does not
	// match the sender, reject.
	check_room_auth_rule_9(event, data);

	// 10. If type is m.room.power_levels:
	if(json::get<"type"_>(event) == "m.room.power_levels")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// 11. If type is m.room.redaction:
	if(json::get<"type"_>(event) == "m.room.redaction")
	{
		room_auth_hook(event, data);
		return {data.allow, data.fail};
	}

	// (non-spec) Call the hook for any types without a branch enumerated
	// here. The handler will throw on a failure, otherwise fallthrough to
	// the next rule.
	room_auth_hook(event, data);

	// 12. Otherwise, allow.
	data.allow = true;
	assert(!data.fail);
	return {data.allow, data.fail};
}
catch(const FAIL &e)
{
	data.allow = false;
	data.fail = std::current_exception();
	return {data.allow, data.fail};
}

//
// m::room::auth internal
//

void
ircd::m::check_room_auth_rule_2(const m::event &event,
                                room::auth::hookdata &data)
{
	using FAIL = room::auth::FAIL;

	// 2. Reject if event has auth_events that:
	for(size_t i(0); i < data.auth_events.size(); ++i)
	{
		// a. have duplicate entries for a given type and state_key pair
		const m::event &a
		{
			*data.auth_events.at(i)
		};

		for(size_t j(0); j < data.auth_events.size(); ++j) if(i != j)
		{
			const m::event &b
			{
				*data.auth_events.at(j)
			};

			if(json::get<"type"_>(a) == json::get<"type"_>(b))
				if(json::get<"state_key"_>(a) == json::get<"state_key"_>(b))
					throw FAIL
					{
						"Duplicate (type,state_key) in auth_events."
					};
		}

		// aa. have auth events that are not in the same room.
		if(at<"room_id"_>(a) != at<"room_id"_>(event))
			throw FAIL
			{
				"Auth event %s in %s cannot be used in %s",
				string_view{a.event_id},
				at<"room_id"_>(a),
				at<"room_id"_>(event),
			};

		// b. have entries whose type and state_key don't match those specified by
		// the auth events selection algorithm described in the server...
		const string_view &type
		{
			json::get<"type"_>(a)
		};

		if(type == "m.room.create")
			continue;

		if(type == "m.room.power_levels")
			continue;

		if(type == "m.room.join_rules")
			continue;

		if(type == "m.room.member")
		{
			if(json::get<"sender"_>(event) == json::get<"state_key"_>(a))
				continue;

			if(json::get<"state_key"_>(event) == json::get<"state_key"_>(a))
				continue;
		}

		throw FAIL
		{
			"Reference in auth_events is not an auth_event."
		};
	}
}

void
ircd::m::check_room_auth_rule_3(const m::event &event,
                                room::auth::hookdata &data)
{
	using FAIL = room::auth::FAIL;

	// 3. If event does not have a m.room.create in its auth_events, reject.
	if(!data.auth_create)
		throw FAIL
		{
			"Missing m.room.create in auth_events."
		};
}

void
ircd::m::check_room_auth_rule_6(const m::event &event,
                                room::auth::hookdata &data)
{
	using FAIL = room::auth::FAIL;

	// 6. If the sender's current membership state is not join, reject.
	if(data.auth_member_sender)
		if(membership(*data.auth_member_sender) != "join")
			throw FAIL
			{
				"sender is not joined to room."
			};
}

void
ircd::m::check_room_auth_rule_8(const m::event &event,
                                room::auth::hookdata &data)
{
	using FAIL = room::auth::FAIL;

	const m::room::power power
	{
		data.auth_power? *data.auth_power : m::event{}, *data.auth_create
	};

	// 8. If the event type's required power level is greater than the
	// sender's power level, reject.
	if(!power(at<"sender"_>(event), "events", at<"type"_>(event), json::get<"state_key"_>(event)))
		throw FAIL
		{
			"sender has insufficient power for event type."
		};
}

void
ircd::m::check_room_auth_rule_9(const m::event &event,
                                room::auth::hookdata &data)
{
	using FAIL = room::auth::FAIL;

	// 9. If the event has a state_key that starts with an @ and does not
	// match the sender, reject.
	if(startswith(json::get<"state_key"_>(event), '@'))
		if(at<"state_key"_>(event) != at<"sender"_>(event))
			throw FAIL
			{
				"sender cannot set another user's mxid in a state_key."
			};
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::is_power_event(const m::event &event)
{
	if(!json::get<"type"_>(event))
		return false;

	if(json::get<"type"_>(event) == "m.room.create")
		return true;

	if(json::get<"type"_>(event) == "m.room.power_levels")
		return true;

	if(json::get<"type"_>(event) == "m.room.join_rules")
		return true;

	if(json::get<"type"_>(event) != "m.room.member")
		return false;

	if(!json::get<"sender"_>(event) || !json::get<"state_key"_>(event))
		return false;

	if(json::get<"sender"_>(event) == json::get<"state_key"_>(event))
		return false;

	if(membership(event) == "leave" || membership(event) == "ban")
		return true;

	return false;
}

//
// room::auth::hookdata
//

IRCD_MODULE_EXPORT
ircd::m::room::auth::hookdata::hookdata(const m::event &event,
                                        const vector_view<const m::event *> &auth_events)
:prev
{
	event
}
,auth_events
{
	auth_events
}
,auth_create
{
	find([](const auto &event)
	{
		return json::get<"type"_>(event) == "m.room.create";
	})
}
,auth_power
{
	find([](const auto &event)
	{
		return json::get<"type"_>(event) == "m.room.power_levels";
	})
}
,auth_join_rules
{
	find([](const auto &event)
	{
		return json::get<"type"_>(event) == "m.room.join_rules";
	})
}
,auth_member_target
{
	find([&event](const auto &auth_event)
	{
		return json::get<"type"_>(auth_event) == "m.room.member" &&
		       json::get<"state_key"_>(auth_event) == json::get<"state_key"_>(event);
	})
}
,auth_member_sender
{
	find([&event](const auto &auth_event)
	{
		return json::get<"type"_>(auth_event) == "m.room.member" &&
		       json::get<"state_key"_>(auth_event) == json::get<"sender"_>(event);
	})
}
{
}

const ircd::m::event *
ircd::m::room::auth::hookdata::find(const event::closure_bool &closure)
const
{
	for(const auto *const &event : auth_events)
		if(likely(event) && closure(*event))
			return event;

	return nullptr;
}

//
// room::auth::refs
//

size_t
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::count()
const noexcept
{
	return count(string_view{});
}

size_t
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::count(const string_view &type)
const noexcept
{
	size_t ret(0);
	for_each(type, [&ret](const auto &)
	{
		++ret;
		return true;
	});

	return ret;
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::has(const event::idx &idx)
const noexcept
{
	return !for_each([&idx](const event::idx &ref)
	{
		return ref != idx; // true to continue, false to break
	});
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::has(const string_view &type)
const noexcept
{
	bool ret{false};
	for_each(type, [&ret](const auto &)
	{
		ret = true;
		return false;
	});

	return ret;
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::for_each(const closure_bool &closure)
const
{
	return for_each(string_view{}, closure);
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::refs::for_each(const string_view &type,
                                     const closure_bool &closure)
const
{
	assert(idx);
	const event::refs erefs
	{
		idx
	};

	return erefs.for_each(dbs::ref::NEXT_AUTH, [this, &type, &closure]
	(const event::idx &ref, const dbs::ref &)
	{
		bool match;
		const auto matcher
		{
			[&type, &match](const string_view &type_)
			{
				match = type == type_;
			}
		};

		if(type)
		{
			if(!m::get(std::nothrow, ref, "type", matcher))
				return true;

			if(!match)
				return true;
		}

		assert(idx != ref);
		if(!closure(ref))
			return false;

		return true;
	});
}

//
// room::auth::chain
//

size_t
IRCD_MODULE_EXPORT
ircd::m::room::auth::chain::depth()
const noexcept
{
	size_t ret(0);
	for_each([&ret](const auto &)
	{
		++ret;
	});

	return ret;
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::chain::has(const string_view &type)
const noexcept
{
	bool ret(false);
	for_each(closure_bool{[&type, &ret]
	(const auto &idx)
	{
		m::get(std::nothrow, idx, "type", [&type, &ret]
		(const auto &value)
		{
			ret = value == type;
		});

		return !ret;
	}});

	return ret;
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::chain::for_each(const closure &closure)
const
{
	return for_each(closure_bool{[&closure]
	(const auto &idx)
	{
		closure(idx);
		return true;
	}});
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::chain::for_each(const closure_bool &closure)
const
{
	return chain::for_each(*this, closure);
}

bool
IRCD_MODULE_EXPORT
ircd::m::room::auth::chain::for_each(const auth::chain &c,
                                      const closure_bool &closure)
{
	m::event::fetch e, a;
	std::set<event::idx> ae;
	std::deque<event::idx> aq {c.idx}; do
	{
		const auto idx(aq.front());
		aq.pop_front();
		if(!seek(e, idx, std::nothrow))
			continue;

		const m::event::prev prev{e};
		const auto count(prev.auth_events_count());
		for(size_t i(0); i < count && i < 4; ++i)
		{
			const auto &auth_event_id(prev.auth_event(i));
			const auto &auth_event_idx(index(auth_event_id, std::nothrow));
			if(!auth_event_idx)
				continue;

			auto it(ae.lower_bound(auth_event_idx));
			if(it == end(ae) || *it != auth_event_idx)
			{
				seek(a, auth_event_idx, std::nothrow);
				ae.emplace_hint(it, auth_event_idx);
				if(a.valid)
					aq.emplace_back(auth_event_idx);
			}
		}
	}
	while(!aq.empty());

	for(const auto &idx : ae)
		if(!closure(idx))
			return false;

	return true;
}