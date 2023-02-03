/* -----------------------------------------------------------------------------
 *
 * Giada - Your Hardcore Loopmachine
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2023 Giovanni A. Zuliani | Monocasual Laboratories
 *
 * This file is part of Giada - Your Hardcore Loopmachine.
 *
 * Giada - Your Hardcore Loopmachine is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Giada - Your Hardcore Loopmachine is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Giada - Your Hardcore Loopmachine. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------- */

#include "core/actions/actionRecorder.h"
#include "core/actions/action.h"
#include "core/actions/actionFactory.h"
#include "core/actions/actions.h"
#include "core/const.h"
#include "core/model/model.h"
#include "core/patch.h"
#include "utils/log.h"
#include "utils/ver.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <unordered_map>

namespace giada::m
{
namespace
{
constexpr int MAX_LIVE_RECS_CHUNK = 128;
} // namespace

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

ActionRecorder::ActionRecorder(model::Model& m)
: m_model(m)
, m_actions(m)
{
	m_liveActions.reserve(MAX_LIVE_RECS_CHUNK);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::reset()
{
	m_liveActions.clear();
	actionFactory::reset();
	m_actions.clearAll();
}

/* -------------------------------------------------------------------------- */

bool ActionRecorder::isBoundaryEnvelopeAction(const Action& a) const
{
	assert(a.prev != nullptr);
	assert(a.next != nullptr);
	return a.prev->frame > a.frame || a.next->frame < a.frame;
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateBpm(float ratio, int quantizerStep)
{
	if (ratio == 1.0f)
		return;

	m_actions.updateKeyFrames([=](Frame old) {
		/* The division here cannot be precise. A new frame can be 44099 and the 
		quantizer set to 44100. That would mean two recs completely useless. So we 
		compute a reject value ('delta'): if it's lower than 6 frames the new frame 
		is collapsed with a quantized frame. FIXME - maybe 6 frames are too low. */
		Frame frame = static_cast<Frame>(old * ratio);
		if (frame != 0)
		{
			Frame delta = quantizerStep % frame;
			if (delta > 0 && delta <= 6)
				frame = frame + delta;
		}
		return frame;
	});
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateSamplerate(int systemRate, int patchRate)
{
	if (systemRate == patchRate)
		return;

	float ratio = systemRate / (float)patchRate;

	m_actions.updateKeyFrames([=](Frame old) { return floorf(old * ratio); });
}

/* -------------------------------------------------------------------------- */

bool ActionRecorder::cloneActions(ID channelId, ID newChannelId)
{
	bool                       cloned = false;
	std::vector<Action>        actions;
	std::unordered_map<ID, ID> map; // Action ID mapper, old -> new

	m_actions.forEachAction([&](const Action& a) {
		if (a.channelId != channelId)
			return;

		ID newActionId = actionFactory::getNewActionId();

		map.insert({a.id, newActionId});

		Action clone(a);
		clone.id        = newActionId;
		clone.channelId = newChannelId;

		actions.push_back(clone);
		cloned = true;
	});

	/* Update nextId and prevId relationships given the new action ID. */

	for (Action& a : actions)
	{
		if (a.prevId != 0)
			a.prevId = map.at(a.prevId);
		if (a.nextId != 0)
			a.nextId = map.at(a.nextId);
	}

	m_actions.rec(actions);

	m_model.get().getChannel(newChannelId).hasActions = true;
	m_model.swap(model::SwapType::HARD);

	return cloned;
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::liveRec(ID channelId, MidiEvent e, Frame globalFrame)
{
	assert(e.isNoteOnOff()); // Can't record any other kind of events for now

	/* TODO - this might allocate on the MIDI thread */
	if (m_liveActions.size() >= m_liveActions.capacity())
		m_liveActions.reserve(m_liveActions.size() + MAX_LIVE_RECS_CHUNK);

	m_liveActions.push_back(actionFactory::makeAction(actionFactory::getNewActionId(), channelId, globalFrame, e));
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::recordEnvelopeAction(ID channelId, Frame frame, int value, Frame lastFrameInLoop)
{
	assert(value >= 0 && value <= G_MAX_VELOCITY);

	/* First action ever? Add actions at boundaries. Else, find action right
	before frame 'f' and inject a new action in there. Vertical envelope points 
	are forbidden for now. */

	if (!hasActions(channelId, MidiEvent::CHANNEL_CC))
		recordFirstEnvelopeAction(channelId, frame, value, lastFrameInLoop);
	else
		recordNonFirstEnvelopeAction(channelId, frame, value);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::recordMidiAction(ID channelId, int note, int velocity, Frame f1, Frame f2, Frame framesInLoop)
{
	if (f2 == 0)
		f2 = f1 + G_DEFAULT_ACTION_SIZE;

	/* Avoid frame overflow. */

	Frame overflow = f2 - framesInLoop;
	if (overflow > 0)
	{
		f2 -= overflow;
		f1 -= overflow;
	}

	MidiEvent e1 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_NOTE_ON, note, velocity);
	MidiEvent e2 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_NOTE_OFF, note, velocity);

	rec(channelId, f1, f2, e1, e2);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::recordSampleAction(ID channelId, int type, Frame f1, Frame f2)
{
	if (isSinglePressMode(channelId))
	{
		if (f2 == 0)
			f2 = f1 + G_DEFAULT_ACTION_SIZE;
		MidiEvent e1 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_NOTE_ON, 0, 0);
		MidiEvent e2 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_NOTE_OFF, 0, 0);
		rec(channelId, f1, f2, e1, e2);
	}
	else
	{
		MidiEvent e1 = MidiEvent::makeFrom3Bytes(type, 0, 0);
		rec(channelId, f1, e1);
	}
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::deleteMidiAction(ID channelId, const Action& a)
{
	assert(a.isValid());
	assert(a.event.getStatus() == MidiEvent::CHANNEL_NOTE_ON);

	/* Check if 'next' exist first: could be orphaned. */

	if (a.next != nullptr)
		deleteAction(channelId, a.id, a.next->id);
	else
		deleteAction(channelId, a.id);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::deleteSampleAction(ID channelId, const Action& a)
{
	if (a.next != nullptr) // For ChannelMode::SINGLE_PRESS combo
		deleteAction(channelId, a.id, a.next->id);
	else
		deleteAction(channelId, a.id);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::deleteEnvelopeAction(ID channelId, const Action& a)
{
	/* Deleting a boundary action wipes out everything. */
	/* TODO - FIX*/

	if (isBoundaryEnvelopeAction(a))
	{
		if (a.isVolumeEnvelope())
		{
			// TODO reset all channel's volume vars to 1.0
		}
		clearActions(channelId, a.event.getStatus());
	}
	else
	{
		assert(a.prev != nullptr);
		assert(a.next != nullptr);

		const Action a1     = *a.prev;
		const Action a1prev = *a1.prev;
		const Action a3     = *a.next;
		const Action a3next = *a3.next;

		/* Original status:   a1--->a--->a3
		   Modified status:   a1-------->a3 
		Order is important, here: first update siblings, then delete the action.
		Otherwise ActionRecorder::deleteAction() would complain of missing 
		prevId/nextId no longer found. */

		updateSiblings(a1.id, a1prev.id, a3.id);
		updateSiblings(a3.id, a1.id, a3next.id);
		deleteAction(channelId, a.id);
	}
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateMidiAction(ID channelId, const Action& a, int note, int velocity,
    Frame f1, Frame f2, Frame framesInLoop)
{
	deleteAction(channelId, a.id, a.next->id);
	recordMidiAction(channelId, note, velocity, f1, f2, framesInLoop);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateSampleAction(ID channelId, const Action& a, int type, Frame f1, Frame f2)
{
	if (isSinglePressMode(channelId))
		deleteAction(channelId, a.id, a.next->id);
	else
		deleteAction(channelId, a.id);

	recordSampleAction(channelId, type, f1, f2);
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateEnvelopeAction(ID channelId, const Action& a, Frame f, int value, Frame lastFrameInLoop)
{
	/* Update the action directly if it is a boundary one. Else, delete the
	previous one and record a new action. */

	if (isBoundaryEnvelopeAction(a))
		updateEvent(a.id, MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_CC, 0, value));
	else
	{
		deleteEnvelopeAction(channelId, a);
		recordEnvelopeAction(channelId, f, value, lastFrameInLoop);
	}
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::updateVelocity(const Action& a, int value)
{
	MidiEvent event(a.event);
	event.setVelocity(value);

	updateEvent(a.id, event);
}

/* -------------------------------------------------------------------------- */

std::unordered_set<ID> ActionRecorder::consolidate()
{
	for (auto it = m_liveActions.begin(); it != m_liveActions.end(); ++it)
		consolidate(*it, it - m_liveActions.begin()); // Pass current index

	m_actions.rec(m_liveActions);

	std::unordered_set<ID> out;
	for (const Action& action : m_liveActions)
		out.insert(action.channelId);

	m_liveActions.clear();
	return out;
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::clearAllActions()
{
	for (Channel& ch : m_model.get().channels)
		ch.hasActions = false;
	m_model.swap(model::SwapType::HARD);

	m_actions.clearAll();
}

/* -------------------------------------------------------------------------- */

bool ActionRecorder::areComposite(const Action& a1, const Action& a2) const
{
	return a1.event.getStatus() == MidiEvent::CHANNEL_NOTE_ON &&
	       a2.event.getStatus() == MidiEvent::CHANNEL_NOTE_OFF &&
	       a1.event.getNote() == a2.event.getNote() &&
	       a1.channelId == a2.channelId;
}

/* -------------------------------------------------------------------------- */

Frame ActionRecorder::fixVerticalEnvActions(Frame f, const Action& a1, const Action& a2) const
{
	if (a1.frame == f)
		f += 1;
	else if (a2.frame == f)
		f -= 1;
	if (a1.frame == f || a2.frame == f)
		return -1;
	return f;
}

/* -------------------------------------------------------------------------- */

bool ActionRecorder::isSinglePressMode(ID channelId) const
{
	return m_model.get().getChannel(channelId).samplePlayer->mode == SamplePlayerMode::SINGLE_PRESS;
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::consolidate(const Action& a1, std::size_t i)
{
	/* This algorithm must start searching from the element next to 'a1': since 
	live actions are recorded in linear sequence, the potential partner of 'a1' 
	always lies beyond a1 itself. Without this trick (i.e. if it loops from 
	vector.begin() each time) the algorithm would end up matching wrong partners. */

	for (auto it = m_liveActions.begin() + i; it != m_liveActions.end(); ++it)
	{

		const Action& a2 = *it;

		if (!areComposite(a1, a2))
			continue;

		const_cast<Action&>(a1).nextId = a2.id;
		const_cast<Action&>(a2).prevId = a1.id;

		break;
	}
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::recordFirstEnvelopeAction(ID channelId, Frame frame, int value, Frame lastFrameInLoop)
{
	// TODO - use MidiEvent's float velocity
	const MidiEvent e1 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_CC, 0, G_MAX_VELOCITY);
	const MidiEvent e2 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_CC, 0, value);

	const Action a1 = rec(channelId, 0, e1);
	const Action a2 = rec(channelId, frame, e2);
	const Action a3 = rec(channelId, lastFrameInLoop, e1);

	updateSiblings(a1.id, /*prev=*/a3.id, /*next=*/a2.id); // Circular loop (begin)
	updateSiblings(a2.id, /*prev=*/a1.id, /*next=*/a3.id);
	updateSiblings(a3.id, /*prev=*/a2.id, /*next=*/a1.id); // Circular loop (end)
}

/* -------------------------------------------------------------------------- */

void ActionRecorder::recordNonFirstEnvelopeAction(ID channelId, Frame frame, int value)
{
	const Action a1 = getClosestAction(channelId, frame, MidiEvent::CHANNEL_CC);
	const Action a3 = a1.next != nullptr ? *a1.next : Action{};

	assert(a1.isValid());
	assert(a3.isValid());

	frame = fixVerticalEnvActions(frame, a1, a3);
	if (frame == -1) // Vertical points, nothing to do here
		return;

	// TODO - use MidiEvent's float velocity
	MidiEvent    e2 = MidiEvent::makeFrom3Bytes(MidiEvent::CHANNEL_CC, 0, value);
	const Action a2 = rec(channelId, frame, e2);

	updateSiblings(a2.id, a1.id, a3.id);
}

/* -------------------------------------------------------------------------- */

const std::vector<Action>* ActionRecorder::getActionsOnFrame(Frame f) const
{
	return m_actions.getActionsOnFrame(f);
}

bool ActionRecorder::hasActions(ID channelId, int type) const
{
	return m_actions.hasActions(channelId, type);
}

Action ActionRecorder::getClosestAction(ID channelId, Frame f, int type) const
{
	return m_actions.getClosestAction(channelId, f, type);
}

std::vector<Action> ActionRecorder::getActionsOnChannel(ID channelId) const
{
	return m_actions.getActionsOnChannel(channelId);
}

void ActionRecorder::clearChannel(ID channelId)
{
	m_model.get().getChannel(channelId).hasActions = false;
	m_actions.clearChannel(channelId);
}

void ActionRecorder::clearActions(ID channelId, int type)
{
	m_actions.clearActions(channelId, type);

	m_model.get().getChannel(channelId).hasActions = hasActions(channelId);
	m_model.swap(model::SwapType::HARD);
}

Action ActionRecorder::rec(ID channelId, Frame frame, MidiEvent e)
{
	m_model.get().getChannel(channelId).hasActions = true;
	return m_actions.rec(channelId, frame, e);
}

void ActionRecorder::rec(ID channelId, Frame f1, Frame f2, MidiEvent e1, MidiEvent e2)
{
	m_model.get().getChannel(channelId).hasActions = true;
	return m_actions.rec(channelId, f1, f2, e1, e2);
}

void ActionRecorder::updateSiblings(ID id, ID prevId, ID nextId)
{
	m_actions.updateSiblings(id, prevId, nextId);
}

void ActionRecorder::deleteAction(ID channelId, ID id)
{
	m_actions.deleteAction(id);

	m_model.get().getChannel(channelId).hasActions = hasActions(channelId);
	m_model.swap(model::SwapType::HARD);
}

void ActionRecorder::deleteAction(ID channelId, ID currId, ID nextId)
{
	m_actions.deleteAction(currId, nextId);

	m_model.get().getChannel(channelId).hasActions = hasActions(channelId);
	m_model.swap(model::SwapType::HARD);
}

void ActionRecorder::updateEvent(ID id, MidiEvent e)
{
	m_actions.updateEvent(id, e);
}
} // namespace giada::m
