// @@@LICENSE
//
//	Copyright (c) 2009-2013 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// LICENSE@@@

#include "ActivityManager.h"
#include "ResourceManager.h"
#include "Logging.h"

#include <boost/iterator/transform_iterator.hpp>
#include <cstdlib>
#include <stdexcept>

MojLogger ActivityManager::s_log(_T("activitymanager.activitymanager"));

const char *ActivityManager::RunQueueNames[] = {
	"initialized",
	"scheduled",
	"ready",
	"readyInteractive",
	"background",
	"backgroundInteractive",
	"longBackground",
	"immediate",
	"ended"
};

ActivityManager::ActivityManager(boost::shared_ptr<MasterResourceManager>
	resourceManager)
	: m_enabled(EXTERNAL_ENABLE)
	, m_backgroundConcurrencyLevel(DefaultBackgroundConcurrencyLevel)
	, m_backgroundInteractiveConcurrencyLevel
		(DefaultBackgroundInteractiveConcurrencyLevel)
	, m_yieldTimeoutSeconds(DefaultBackgroundInteractiveYieldSeconds)
	, m_resourceManager(resourceManager)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
#ifndef ACTIVITYMANAGER_RANDOM_IDS
	/* Activity ID 0 is reserved */
	m_nextActivityId = 1;
#endif
}

ActivityManager::~ActivityManager()
{
}

void ActivityManager::RegisterActivityId(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Registering ID", act->GetId());

	ActivityMap::const_iterator found = m_activities.find(act->GetId());
	if (found != m_activities.end()) {
		throw std::runtime_error("Activity ID is already registered");
	}

	m_activities[act->GetId()] = act;
}

void ActivityManager::RegisterActivityName(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Registering as %s/\"%s\"",
		act->GetId(), act->GetCreator().GetString().c_str(),
		act->GetName().c_str());

	bool success = m_nameTable.insert(*act).second;
	if (!success) {
		throw std::runtime_error("Activity name is already registed");
	}
}

void ActivityManager::UnregisterActivityName(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Unregistering from %s/\"%s\"",
		act->GetId(), act->GetCreator().GetString().c_str(),
		act->GetName().c_str());

	if (act->m_nameTableItem.is_linked()) {
		act->m_nameTableItem.unlink();
	} else {
		throw std::runtime_error("Activity name is not registered");
	}
}

boost::shared_ptr<Activity> ActivityManager::GetActivity(
	const std::string& name, const BusId& creator)
{
	ActivityNameTable::iterator iter;

	if (creator.GetType() == BusAnon) {
		iter = m_nameTable.find(ActivityKey(name, creator), ActivityNameOnlyComp());
	} else {
		iter = m_nameTable.find(ActivityKey(name, creator), ActivityNameComp());
	}

	if (iter == m_nameTable.end()) {
		throw std::runtime_error("Activity name/creator pair not found");
	}

	return iter->shared_from_this();
}

boost::shared_ptr<Activity> ActivityManager::GetActivity(activityId_t id)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	ActivityMap::iterator iter = m_activities.find(id);
	if (iter != m_activities.end()) {
		return iter->second;
	}

	throw std::runtime_error("activityId not found");
}

boost::shared_ptr<Activity> ActivityManager::GetNewActivity()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	boost::shared_ptr<Activity> act;

#ifdef ACTIVITYMANAGER_RANDOM_IDS
	/* Look for unused id */
	while (true) {
		activityId_t id = (activityId_t) ::random();

		/* Activity ID 0 is reserved */
		if (id == 0)
			continue;

		/* Either the searched for element, or the first higher one */
		ActivityIdTable::const_iterator hint = m_idTable.lower_bound(id,
			ActivityIdComp());
		if ((hint == m_idTable.end()) || (hint->GetId() != id)) {
			act = boost::make_shared<Activity>(id, shared_from_this());
			m_idTable.insert(hint, *act);
			break;
		}
	}
#else
	/* Use sequential ID */
	while (true) {
		ActivityIdTable::const_iterator hint = m_idTable.lower_bound(
			m_nextActivityId, ActivityIdComp());
		if ((hint == m_idTable.end()) || (hint->GetId() != m_nextActivityId)) {
			act = boost::make_shared<Activity>(m_nextActivityId++,
				shared_from_this());
			m_idTable.insert(hint, *act);
			break;
		} else {
			m_nextActivityId++;
		}
	}
#endif	/* ACTIVITYMANAGER_RANDOM_IDS */

	LOG_AM_DEBUG("[Activity %llu] Allocated", act->GetId());

	return act;
}

boost::shared_ptr<Activity> ActivityManager::GetNewActivity(activityId_t id)
{
	LOG_AM_DEBUG("[Activity %llu] Forcing allocation", id);

	ActivityMap::iterator iter = m_activities.find(id);
	if (iter != m_activities.end()) {
		LOG_AM_WARNING(MSGID_SAME_ACTIVITY_ID_FOUND, 1, PMLOGKFV("Activity","%llu",id), "");
	}

	boost::shared_ptr<Activity> act = boost::make_shared<Activity>(id,
		shared_from_this());
	m_idTable.insert(*act);

	return act;
}

void ActivityManager::ReleaseActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Releasing", act->GetId());

	EvictQueue(act);

	ActivityMap::iterator iter = m_activities.find(act->GetId());
	if (iter == m_activities.end()) {
		LOG_AM_WARNING(MSGID_RELEASE_ACTIVITY_NOTFOUND, 1, PMLOGKFV("Activity","%llu",act->GetId()),
			"Not found in Activity table while attempting to release");
	} else {
		if (iter->second == act) {
			m_activities.erase(iter);
		}
	}

	CheckReadyQueue();
}

ActivityManager::ActivityVec ActivityManager::GetActivities() const
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	ActivityVec out;
	out.reserve(m_activities.size());

	std::transform(m_activities.begin(), m_activities.end(),
		std::back_inserter(out),
		boost::bind(&ActivityMap::value_type::second, _1));

	return out;
}

MojErr ActivityManager::StartActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Start", act->GetId());

	act->SendCommand(ActivityStartCommand);

	return MojErrNone;
}

MojErr ActivityManager::StopActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Stop", act->GetId());

	act->SendCommand(ActivityStopCommand);

	return MojErrNone;
}

MojErr ActivityManager::CancelActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Cancel", act->GetId());

	act->SendCommand(ActivityCancelCommand);

	return MojErrNone;
}

MojErr ActivityManager::PauseActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Pause", act->GetId());

	act->SendCommand(ActivityPauseCommand);

	return MojErrNone;
}

MojErr ActivityManager::FocusActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Focus [Activity %llu]", act->GetId());

	if (act->IsFocused()) {
		LOG_AM_DEBUG("[Activity %llu] is already focused",
			act->GetId());
		return MojErrNone;
	}

	act->SetFocus(true);
	m_resourceManager->UpdateAssociations(act);

	ActivityFocusedList oldFocused;
	oldFocused.swap(m_focusedActivities);

	m_focusedActivities.push_back(*act);

	/* Remove focus from all Activities that had focus before. */
	while (!oldFocused.empty()) {
		Activity& focusedActivity = oldFocused.front();
		LOG_AM_DEBUG("Removing focus from previously focused [Activity %llu]",
			focusedActivity.GetId());
		focusedActivity.SetFocus(false);
		m_resourceManager->UpdateAssociations(
			focusedActivity.shared_from_this());
		oldFocused.pop_front();
	}

	return MojErrNone;
}

MojErr ActivityManager::UnfocusActivity(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Unfocus [Activity %llu]", act->GetId());

	if (!act->IsFocused()) {
		LOG_AM_WARNING(MSGID_UNFOCUS_ACTIVITY_FAILED, 1, PMLOGKFV("Activity","%llu",act->GetId()),
			    "Can't remove focus from activity which is not focused");
		return MojErrInvalidArg;
	}

	act->SetFocus(false);
	m_resourceManager->UpdateAssociations(act);

	if (act->m_focusedListItem.is_linked()) {
		act->m_focusedListItem.unlink();
	} else {
		LOG_AM_WARNING(MSGID_ACTIVITY_NOT_ON_FOCUSSED_LIST, 1, PMLOGKFV("Activity","%llu",act->GetId()),
			    "activity not on focus list while removing focus");
	}

	return MojErrNone;
}

MojErr ActivityManager::AddFocus(boost::shared_ptr<Activity> source,
	boost::shared_ptr<Activity> target)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Add focus from [Activity %llu] to [Activity %llu]",
		source->GetId(), target->GetId());

	if (!source->IsFocused()) {
		LOG_AM_WARNING(MSGID_SRC_ACTIVITY_UNFOCUSSED, 2, PMLOGKFV("src_activity","%llu",source->GetId()),
			    PMLOGKFV("target_activity","%llu",target->GetId()),
			    "Can't add focus from src_activity to target_activity as source is not focused");
		return MojErrInvalidArg;
	}

	if (target->IsFocused()) {
		LOG_AM_DEBUG("Target is already focused adding focus from [Activity %llu] to [Activity %llu]",
			source->GetId(), target->GetId());
		return MojErrNone;
	}

	target->SetFocus(true);
	m_resourceManager->UpdateAssociations(target);
	m_focusedActivities.push_back(*target);

	return MojErrNone;
}

void ActivityManager::Enable(unsigned mask)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	if (mask & EXTERNAL_ENABLE) {
		LOG_AM_DEBUG("Enabling Activity Manager: External");
	}

	if (mask & UI_ENABLE) {
		LOG_AM_DEBUG("Enabling Activity Manager: Device UI enabled");
	}

	if ((mask & ENABLE_MASK) != mask) {
		LOG_AM_DEBUG("Unknown bits set in mask in call to Enable: %x", mask);
	}

	m_enabled |= (mask & ENABLE_MASK);

	if (IsEnabled()) {
		ScheduleAllActivities();
	}
}

void ActivityManager::Disable(unsigned mask)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	if (mask & EXTERNAL_ENABLE) {
		LOG_AM_DEBUG("Disabling Activity Manager: External");
	}

	if (mask & UI_ENABLE) {
		LOG_AM_DEBUG("Disabling Activity Manager: Device UI disabled");
	}

	if ((mask & ENABLE_MASK) != mask) {
		LOG_AM_DEBUG("Unknown bits set in mask in call to Disable: %x", mask);
	}

	m_enabled &= ~mask;
}

bool ActivityManager::IsEnabled() const
{
	/* All bits must be enabled */
	return (m_enabled & ENABLE_MASK) == ENABLE_MASK;
}

#ifdef ACTIVITYMANAGER_DEVELOPER_METHODS

unsigned int ActivityManager::SetBackgroundConcurrencyLevel(unsigned int level)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	if (level != UnlimitedBackgroundConcurrency) {
		LOG_AM_DEBUG("Background concurrency level set to %u",
			level);
	} else {
		LOG_AM_DEBUG("Background concurrency level set to Unlimited");
	}

	unsigned int oldLevel = m_backgroundConcurrencyLevel;
	m_backgroundConcurrencyLevel = level;

	/* May want to run more Background Activities */
	CheckReadyQueue();

	return oldLevel;
}

void ActivityManager::EvictBackgroundActivity(
	boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Attempting to evict [Activity %llu] from background queue",
		act->GetId());

	ActivityRunQueue::iterator iter;

	for (iter = m_runQueue[RunQueueBackground].begin();
		iter != m_runQueue[RunQueueBackground].end(); ++iter) {
		if (&(*iter) == &(*act)) {
			break;
		}
	}

	if (iter != m_runQueue[RunQueueBackground].end()) {
		act->m_runQueueItem.unlink();
		m_runQueue[RunQueueLongBackground].push_back(*act);
	} else {
		LOG_AM_ERROR(MSGID_ACTIVITY_NOT_ON_BACKGRND_Q, 1, PMLOGKFV("Activity","%llu",act->GetId()), "");
		throw std::runtime_error("Activity not on background queue");
	}

	CheckReadyQueue();
}

void ActivityManager::EvictAllBackgroundActivities()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Evicting all background Activities to the long running background Activity list");

	while (!m_runQueue[RunQueueBackground].empty()) {
		Activity& act = m_runQueue[RunQueueBackground].front();
		act.m_runQueueItem.unlink();
		m_runQueue[RunQueueLongBackground].push_back(act);
	}

	CheckReadyQueue();
}

void ActivityManager::RunReadyBackgroundActivity(
	boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Attepting to run ready [Activity %llu]",
		act->GetId());

	ActivityRunQueue::iterator iter;
	for (iter = m_runQueue[RunQueueReady].begin();
		iter != m_runQueue[RunQueueReady].end(); ++iter) {
		if (&(*iter) == &(*act)) {
			break;
		}
	}

	if (iter != m_runQueue[RunQueueReady].end()) {
		RunReadyBackgroundActivity(*iter);
		return;
	}

	for (iter = m_runQueue[RunQueueReadyInteractive].begin();
		iter != m_runQueue[RunQueueReadyInteractive].end(); ++iter) {
		if (&(*iter) == &(*act)) {
			break;
		}
	}

	if (iter != m_runQueue[RunQueueReadyInteractive].end()) {
		RunReadyBackgroundInteractiveActivity(*iter);
		return;
	}

	LOG_AM_WARNING(MSGID_ACTIVITY_NOT_ON_READY_Q, 1, PMLOGKFV("Activity","%llu",act->GetId()),
		    "activity not found on ready queue");
	throw std::runtime_error("Activity not on ready queue");
}

void ActivityManager::RunAllReadyActivities()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Running all Activities currently in the Ready state");

	while (!m_runQueue[RunQueueReadyInteractive].empty()) {
		RunReadyBackgroundInteractiveActivity(
			m_runQueue[RunQueueReadyInteractive].front());
	}

	while (!m_runQueue[RunQueueReady].empty()) {
		RunReadyBackgroundActivity(m_runQueue[RunQueueReady].front());
	}
}

#endif /* ACTIVITYMANAGER_DEVELOPER_METHODS */

void ActivityManager::InformActivityInitialized(
	boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Initialized and ready to be scheduled",
		act->GetId());

	/* If an Activity is restarting, it will be parked (temporarily) in
	 * the ended queue. */
	if (act->m_runQueueItem.is_linked()) {
		act->m_runQueueItem.unlink();
	}

	/* If the Activity Manager isn't enabled yet, just queue the Activities.
	 * otherwise, schedule them immediately. */
	if (IsEnabled()) {
		m_runQueue[RunQueueScheduled].push_back(*act);
		act->ScheduleActivity();
	} else {
		m_runQueue[RunQueueInitialized].push_back(*act);
	}
}

void ActivityManager::InformActivityReady(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Now ready to run", act->GetId());

	if (act->m_runQueueItem.is_linked()) {
		act->m_runQueueItem.unlink();
	} else {
		LOG_AM_DEBUG("[Activity %llu] not found on any run queue when moving to ready state",
			act->GetId());
	}

	if (act->IsImmediate()) {
		m_runQueue[RunQueueImmediate].push_back(*act);
		RunActivity(*act);
	} else {
		if (act->IsUserInitiated()) {
			m_runQueue[RunQueueReadyInteractive].push_back(*act);
		} else {
			m_runQueue[RunQueueReady].push_back(*act);
		}

		CheckReadyQueue();
	}
}

void ActivityManager::InformActivityNotReady(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] No longer ready to run",
		act->GetId());

	if (act->m_runQueueItem.is_linked()) {
		act->m_runQueueItem.unlink();
	} else {
		LOG_AM_DEBUG("[Activity %llu] not found on any run queue when moving to not ready state",
			act->GetId());
	}

	m_runQueue[RunQueueScheduled].push_back(*act);
}

void ActivityManager::InformActivityRunning(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Running", act->GetId());
}

void ActivityManager::InformActivityEnding(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Ending", act->GetId());

	/* Nothing to do here yet, it still has subscribers who may have processing
	 * to do. */
}

void ActivityManager::InformActivityEnd(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Has ended", act->GetId());

	/* If Activity was never fully initialized, it's ok for it not to be on
	 * a queue here */
	if (act->m_runQueueItem.is_linked()) {
		act->m_runQueueItem.unlink();
	}

	m_runQueue[RunQueueEnded].push_back(*act);

	m_resourceManager->Dissociate(act);

	/* Could be room to run more background Activities */
	CheckReadyQueue();
}

void ActivityManager::InformActivityGainedSubscriberId(
	boost::shared_ptr<Activity> act, const BusId& id)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Gained subscriber [BusId %s]",
		act->GetId(), id.GetString().c_str());

	m_resourceManager->Associate(act, id);
}

void ActivityManager::InformActivityLostSubscriberId(
	boost::shared_ptr<Activity> act, const BusId& id)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("[Activity %llu] Lost subscriber [BusId %s]",
		act->GetId(), id.GetString().c_str());

	m_resourceManager->Dissociate(act, id);
}

void ActivityManager::ScheduleAllActivities()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Scheduling all Activities");

	while (!m_runQueue[RunQueueInitialized].empty()) {
		Activity& act = m_runQueue[RunQueueInitialized].front();
		act.m_runQueueItem.unlink();

		LOG_AM_DEBUG("Granting [Activity %llu] permission to schedule",
			act.GetId());

		m_runQueue[RunQueueScheduled].push_back(act);
		act.ScheduleActivity();
	}
}

void ActivityManager::EvictQueue(boost::shared_ptr<Activity> act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	if (act->m_runQueueItem.is_linked()) {
		LOG_AM_DEBUG("[Activity %llu] evicted from run queue on release",
			act->GetId());
		act->m_runQueueItem.unlink();
	}
}

void ActivityManager::RunActivity(Activity& act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Running [Activity %llu]", act.GetId());

	m_resourceManager->Associate(act.shared_from_this());
	act.RunActivity();
}

void ActivityManager::RunReadyBackgroundActivity(Activity& act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Running background [Activity %llu]", act.GetId());

	if (act.m_runQueueItem.is_linked()) {
		act.m_runQueueItem.unlink();
	} else {
		LOG_AM_WARNING(MSGID_ATTEMPT_RUN_BACKGRND_ACTIVITY, 1, PMLOGKFV("Activity","%llu",act.GetId()), "");
	}

	m_runQueue[RunQueueBackground].push_back(act);

	RunActivity(act);
}

void ActivityManager::RunReadyBackgroundInteractiveActivity(Activity& act)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Running background interactive [Activity %llu]",
		act.GetId());

	if (act.m_runQueueItem.is_linked()) {
		act.m_runQueueItem.unlink();
	} else {
		LOG_AM_DEBUG("[Activity %llu] was not queued attempting to run background interactive Activity",
			 act.GetId());
	}

	m_runQueue[RunQueueBackgroundInteractive].push_back(act);

	RunActivity(act);
}

unsigned ActivityManager::GetRunningBackgroundActivitiesCount() const
{
	return (unsigned) (m_runQueue[RunQueueBackground].size() +
		m_runQueue[RunQueueBackgroundInteractive].size());
}

void ActivityManager::CheckReadyQueue()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Checking to see if more background Activities can run");

	bool ranInteractive = false;

	while (((GetRunningBackgroundActivitiesCount()
				< m_backgroundInteractiveConcurrencyLevel) ||
			(m_backgroundInteractiveConcurrencyLevel
				== UnlimitedBackgroundConcurrency))
			&& !m_runQueue[RunQueueReadyInteractive].empty()) {
		RunReadyBackgroundInteractiveActivity(
			m_runQueue[RunQueueReadyInteractive].front());
		ranInteractive = true;
	}

	if (!m_runQueue[RunQueueReadyInteractive].empty()) {
		if (ranInteractive) {
			UpdateYieldTimeout();
		} else if (!m_interactiveYieldTimeout) {
			UpdateYieldTimeout();
		}
	} else {
		if (m_interactiveYieldTimeout) {
			CancelYieldTimeout();
		}
	}

	while (((GetRunningBackgroundActivitiesCount()
				< m_backgroundConcurrencyLevel) ||
			(m_backgroundConcurrencyLevel == UnlimitedBackgroundConcurrency))
			&& !m_runQueue[RunQueueReady].empty()) {
		RunReadyBackgroundActivity(m_runQueue[RunQueueReady].front());
	}
}

void ActivityManager::UpdateYieldTimeout()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	if (!m_interactiveYieldTimeout) {
		LOG_AM_DEBUG("Arming background interactive yield timeout for %u seconds",
			m_yieldTimeoutSeconds);
	} else {
		LOG_AM_DEBUG("Updating background interactive yield timeout for %u seconds",
			m_yieldTimeoutSeconds);
	}

	m_interactiveYieldTimeout = boost::make_shared<Timeout<ActivityManager> >
		(shared_from_this(), m_yieldTimeoutSeconds,
			&ActivityManager::InteractiveYieldTimeout);
	m_interactiveYieldTimeout->Arm();
}

void ActivityManager::CancelYieldTimeout()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Cancelling background interactive yield timeout");

	m_interactiveYieldTimeout.reset();
}

void ActivityManager::InteractiveYieldTimeout()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Background interactive yield timeout triggered");

	if (m_runQueue[RunQueueReadyInteractive].empty()) {
		LOG_AM_DEBUG("Ready interactive queue is empty, cancelling yield timeout");
		CancelYieldTimeout();
		return;
	}

	/* XXX make 1 more Activity yield, but only if there are fewer Activities
	 * yielding than waiting in the interactive queue. */
	unsigned waiting = (unsigned) m_runQueue[RunQueueReadyInteractive].size();
	unsigned yielding = 0;

	boost::shared_ptr<Activity> victim;

	ActivityRunQueue::iterator iter;

	for (iter = m_runQueue[RunQueueBackgroundInteractive].begin();
		iter != m_runQueue[RunQueueBackgroundInteractive].end(); ++iter) {
		if (iter->IsYielding()) {
			if (++yielding >= waiting) {
				break;
			}
		} else {
			if (!victim) {
				victim = iter->shared_from_this();
			}
		}
	}

	/* If there aren't already too many yielding... */
	if (iter == m_runQueue[RunQueueBackgroundInteractive].end()) {
		if (victim) {
			LOG_AM_DEBUG("Requesting that [Activity %llu] yield",
				victim->GetId());
			victim->YieldActivity();
		} else {
			LOG_AM_DEBUG("All running background interactive Activities are already yielding");
		}
	} else {
		LOG_AM_DEBUG("Number of yielding Activities is already equal to the number of ready interactive Activities waiting in the queue");
	}

	UpdateYieldTimeout();
}

bool ActivityManager::ActivityNameComp::operator()(
	const Activity& act1, const Activity& act2) const
{
	const std::string& act1Name = act1.GetName();
	const std::string& act2Name = act2.GetName();

	if (act1Name != act2Name) {
		return act1Name < act2Name;
	} else {
		return act1.GetCreator() < act2.GetCreator();
	}
}

bool ActivityManager::ActivityNameComp::operator()(
	const ActivityKey& key, const Activity& act) const
{
	return key < ActivityKey(act.GetName(), act.GetCreator());
}

bool ActivityManager::ActivityNameComp::operator()(
	const Activity& act, const ActivityKey& key) const
{
	return ActivityKey(act.GetName(), act.GetCreator()) < key;
}

bool ActivityManager::ActivityNameOnlyComp::operator()(
	const ActivityKey& key, const Activity& act) const
{
	return key.first < act.GetName();
}

bool ActivityManager::ActivityNameOnlyComp::operator()(
	const Activity& act, const ActivityKey& key) const
{
	return act.GetName() < key.first;
}

bool ActivityManager::ActivityIdComp::operator()(
	const Activity& act1, const Activity& act2) const
{
	return act1.GetId() < act2.GetId();
}

bool ActivityManager::ActivityIdComp::operator()(
	const activityId_t& id, const Activity& act) const
{
	return id < act.GetId();
}

bool ActivityManager::ActivityIdComp::operator()(
	const Activity& act, const activityId_t& id) const
{
	return act.GetId() < id;
}

MojErr ActivityManager::InfoToJson(MojObject& rep) const
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	MojErr err = MojErrNone;

	/* Scan the various run queues of the Activity Manager */
	MojObject queues(MojObject::TypeArray);

	for (int i = 0; i < RunQueueMax; i++) {
		if (!m_runQueue[i].empty()) {
			MojObject activities(MojObject::TypeArray);

			std::for_each(m_runQueue[i].begin(), m_runQueue[i].end(),
				boost::bind(&Activity::PushIdentityJson, _1,
					boost::ref(activities)));

			MojObject queue;
			err = queue.putString(_T("name"), RunQueueNames[i]);
			MojErrCheck(err);

			err = queue.put(_T("activities"), activities);
			MojErrCheck(err);

			err = queues.push(queue);
			MojErrCheck(err);
		}
	}

	if (!queues.empty()) {
		err = rep.put(_T("queues"), queues);
		MojErrCheck(err);
	}

	std::vector<boost::shared_ptr<const Activity> > leaked;

	boost::shared_ptr<const Activity> (Activity::*shared_from_this_ptr) () const = &Activity::shared_from_this;
	std::set_difference(
		boost::make_transform_iterator(m_idTable.cbegin(),
			boost::bind(shared_from_this_ptr, _1)),
		boost::make_transform_iterator(m_idTable.cend(),
			boost::bind(shared_from_this_ptr, _1)),
		boost::make_transform_iterator(m_activities.begin(),
			boost::bind(&ActivityMap::value_type::second, _1)),
		boost::make_transform_iterator(m_activities.end(),
			boost::bind(&ActivityMap::value_type::second, _1)),
		std::back_inserter(leaked));

	if (!leaked.empty()) {
		MojObject leakedActivities(MojObject::TypeArray);

		std::for_each(leaked.begin(), leaked.end(),
			boost::bind(&Activity::PushIdentityJson, _1,
				boost::ref(leakedActivities)));

		err = rep.put(_T("leakedActivities"), leakedActivities);
		MojErrCheck(err);
	}

	return MojErrNone;
}

