// @@@LICENSE
//
//      Copyright (c) 2009-2013 LG Electronics, Inc.
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

#include "ConnectionManagerProxy.h"
#include "Activity.h"
#include "MojoCall.h"
#include "Logging.h"

#include <core/MojServiceRequest.h>

#include <stdexcept>

MojLogger ConnectionManagerProxy::s_log(_T("activitymanager.connectionproxy"));

ConnectionManagerProxy::ConnectionManagerProxy(MojService *service)
	: m_service(service)
	, m_internetConfidence(ConnectionConfidenceUnknown)
	, m_wifiConfidence(ConnectionConfidenceUnknown)
	, m_wanConfidence(ConnectionConfidenceUnknown)
{
	m_internetRequirementCore = boost::make_shared<RequirementCore>
		("internet", true);
	m_wifiRequirementCore = boost::make_shared<RequirementCore>
		("wifi", true);
	m_wanRequirementCore = boost::make_shared<RequirementCore>
		("wan", true);

	MojErr err = ConnectionConfidenceUnknownName.assign("unknown");
	if (err != MojErrNone) {
		throw std::runtime_error("Error initializing \"unknown\" connection "
			"confidence name string");
	}

	const char *nameStrings[ConnectionConfidenceMax] =
		{ "none", "poor", "fair", "excellent" };

	for (int i = 0; i < ConnectionConfidenceMax; ++i) {
		MojErr err = ConnectionConfidenceNames[i].assign(nameStrings[i]);
		if (err != MojErrNone) {
			throw std::runtime_error("Error initializing connection confidence "
				"name strings");
		}

		m_internetConfidenceCores[i] = boost::make_shared<RequirementCore>
			("internetConfidence", ConnectionConfidenceNames[i]);
		m_wanConfidenceCores[i] = boost::make_shared<RequirementCore>
			("wanConfidence", ConnectionConfidenceNames[i]);
		m_wifiConfidenceCores[i] = boost::make_shared<RequirementCore>
			("wifiConfidence", ConnectionConfidenceNames[i]);
	}
}

ConnectionManagerProxy::~ConnectionManagerProxy()
{
}

const std::string& ConnectionManagerProxy::GetName() const
{
	static std::string name("ConnectionManagerProxy");
	return name;
}

boost::shared_ptr<Requirement> ConnectionManagerProxy::InstantiateRequirement(
	boost::shared_ptr<Activity> activity, const std::string& name,
	const MojObject& value)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Instantiating [Requirement %s] for [Activity %llu]",
		name.c_str(), activity->GetId());

	if (name == "internet") {
		if ((value.type() == MojObject::TypeBool) && value.boolValue()) {
			boost::shared_ptr<ListedRequirement> req =
				boost::make_shared<BasicCoreListedRequirement>(
					activity, m_internetRequirementCore,
					m_internetRequirementCore->IsMet());
			m_internetRequirements.push_back(*req);
			return req;
		} else {
			throw std::runtime_error("If an 'internet' requirement is "
				"specified, the only legal value is 'true'");
		}
	} else if (name == "internetConfidence") {
		return InstantiateConfidenceRequirement(activity,
			m_internetConfidenceCores, m_internetConfidenceRequirements,
			value);
	} else if (name == "wan") {
		if ((value.type() == MojObject::TypeBool) && value.boolValue()) {
			boost::shared_ptr<ListedRequirement> req =
				boost::make_shared<BasicCoreListedRequirement>(
					activity, m_wanRequirementCore,
					m_wanRequirementCore->IsMet());
			m_wanRequirements.push_back(*req);
			return req;
		} else {
			throw std::runtime_error("If an 'wan' requirement is "
				"specified, the only legal value is 'true'");
		}
	} else if (name == "wanConfidence") {
		return InstantiateConfidenceRequirement(activity, m_wanConfidenceCores,
			m_wanConfidenceRequirements, value);
	} else if (name == "wifi") {
		if ((value.type() == MojObject::TypeBool) && value.boolValue()) {
			boost::shared_ptr<ListedRequirement> req =
				boost::make_shared<BasicCoreListedRequirement>(
					activity, m_wifiRequirementCore,
					m_wifiRequirementCore->IsMet());
			m_wifiRequirements.push_back(*req);
			return req;
		} else {
			throw std::runtime_error("If an 'wifi' requirement is "
				"specified, the only legal value is 'true'");
		}
	} else if (name == "wifiConfidence") {
		return InstantiateConfidenceRequirement(activity, m_wifiConfidenceCores,
			m_wifiConfidenceRequirements, value);
	} else {
		LOG_AM_ERROR(MSGID_REQUIREMENT_INSTANTIATE_FAIL , 3, PMLOGKS("Manager",GetName().c_str()),
			  PMLOGKFV("Activity","%llu",activity->GetId()), PMLOGKS("Requirement",name.c_str()), "");
		throw std::runtime_error("Attempt to instantiate unknown requirement");
	}
}

void ConnectionManagerProxy::RegisterRequirements(
	boost::shared_ptr<MasterRequirementManager> master)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Registering requirements");

	master->RegisterRequirement("internet", shared_from_this());
	master->RegisterRequirement("wifi", shared_from_this());
	master->RegisterRequirement("wan", shared_from_this());
	master->RegisterRequirement("internetConfidence", shared_from_this());
	master->RegisterRequirement("wifiConfidence", shared_from_this());
	master->RegisterRequirement("wanConfidence", shared_from_this());
}

void ConnectionManagerProxy::UnregisterRequirements(
	boost::shared_ptr<MasterRequirementManager> master)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Unregistering requirements");

	master->UnregisterRequirement("internet", shared_from_this());
	master->UnregisterRequirement("wifi", shared_from_this());
	master->UnregisterRequirement("wan", shared_from_this());
	master->UnregisterRequirement("internetConfidence", shared_from_this());
	master->UnregisterRequirement("wifiConfidence", shared_from_this());
	master->UnregisterRequirement("wanConfidence", shared_from_this());
}

void ConnectionManagerProxy::Enable()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Enabling Connection Manager Proxy");

	MojObject params;
	params.putBool(_T("subscribe"), true);

	m_call = boost::make_shared<MojoWeakPtrCall<ConnectionManagerProxy> >(
		boost::dynamic_pointer_cast<ConnectionManagerProxy, RequirementManager>
			(shared_from_this()),
		&ConnectionManagerProxy::ConnectionManagerUpdate, m_service,
		"luna://com.palm.connectionmanager/getstatus", params,
		MojoCall::Unlimited);
	m_call->Call();
}

void ConnectionManagerProxy::Disable()
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);
	LOG_AM_DEBUG("Disabling Connection Manager Proxy");

	m_call.reset();
}

/*
 *luna://com.palm.connectionmanager/getstatus
 *
 * {
 *    "isInternetConnectionAvailable" : <bool>
 *    "wifi" : {
 *        "state" : "connected"|"disconnected"
 *        "ipAddress" : <string>
 *        "ssid" : <string>
 *        "bssid" : <string>
 *    }
 *
 *    "wan" : {
 *        "state" : "connected"|"disconnected"
 *        "ipAddress" : <string>
 *        "network" : "unknown" | "unusable" | "gprs" | "edge" | "umts" ...
 *			(unusable means WAN is available but blocked due to a call)
 *	  }
 *
 * }
 */
void ConnectionManagerProxy::ConnectionManagerUpdate(MojServiceMessage *msg,
	const MojObject& response, MojErr err)
{
	LOG_AM_TRACE("Entering function %s", __FUNCTION__);

	if (err != MojErrNone) {
		if (MojoCall::IsPermanentFailure(msg, response, err)) {
			LOG_AM_WARNING(MSGID_UNSOLVABLE_CONN_MGR_SUBSCR_ERR, 0,
				  "Subscription to Connection Manager experienced an uncorrectable failure: %s",
				  MojoObjectJson(response).c_str());
			m_call.reset();
		} else {
			LOG_AM_WARNING(MSGID_CONN_MGR_SUBSCR_ERR, 0,
				    "Subscription to Connection Manager failed, resubscribing: %s",MojoObjectJson(response).c_str());
			m_call->Call();
		}
		return;
	}

	LOG_AM_DEBUG("Update from Connection Manager: %s",
		MojoObjectJson(response).c_str());

	bool isInternetConnectionAvailable = false;
	response.get(_T("isInternetConnectionAvailable"),
		isInternetConnectionAvailable);

	// We need to pre-process a bit the raw information sent by connectionmanager,
	// which doesn't fit legacy's format completely:
	//  - "wan" is now called "cellular"
	//  - there is a "wired" possible connection
	MojObject internetObj;
	MojObject wifiObj, cellularObj, wiredObj;
	bool foundCellular = response.get(_T("cellular"), cellularObj);
	bool foundWifi = response.get(_T("wifi"), wifiObj);
	bool foundWired = response.get(_T("wired"), wiredObj);

	internetObj.putBool(_T("isInternetConnectionAvailable"), isInternetConnectionAvailable);
	if(foundCellular) {
		internetObj.put(_T("wan"), cellularObj);
	}
	if(foundWifi || foundWired) {
		MojString wifiConnected;
		bool foundWifiState = false;
		if(foundWifi) {
			wifiObj.get(_T("state"), wifiConnected, foundWifiState);
		}

		if(foundWifi && (wifiConnected == "connected" || !foundWired)) {
			internetObj.put(_T("wifi"), wifiObj);
		}
		else if(foundWired && wifiConnected != "connected")
		{
			internetObj.put(_T("wifi"), wiredObj);
		}
	}
	bool updated = m_internetRequirementCore->SetCurrentValue(internetObj);

	if (isInternetConnectionAvailable) {
		if (!m_internetRequirementCore->IsMet()) {
			LOG_AM_DEBUG("Internet connection is now available");
			m_internetRequirementCore->Met();
			std::for_each(m_internetRequirements.begin(),
				m_internetRequirements.end(),
				boost::mem_fn(&Requirement::Met));
		} else if (updated) {
			std::for_each(m_internetRequirements.begin(),
				m_internetRequirements.end(),
				boost::mem_fn(&Requirement::Updated));
		}
	} else {
		if (m_internetRequirementCore->IsMet()) {
			LOG_AM_DEBUG("Internet connection is no longer available");
			m_internetRequirementCore->Unmet();
			std::for_each(m_internetRequirements.begin(),
				m_internetRequirements.end(),
				boost::mem_fn(&Requirement::Unmet));
		}
	}

	UpdateWifiStatus(response);
	UpdateWANStatus(response);

	int maxConfidence = (m_wifiConfidence > m_wanConfidence) ?
		m_wifiConfidence : m_wanConfidence;
	if (m_internetConfidence != maxConfidence) {
		m_internetConfidence = maxConfidence;
		LOG_AM_DEBUG("Internet confidence level changed to %d",
			m_internetConfidence);
		UpdateConfidenceRequirements(m_internetConfidenceCores,
			m_internetConfidenceRequirements, m_internetConfidence);
	}
}

MojErr ConnectionManagerProxy::UpdateWifiStatus(const MojObject& response)
{
	MojErr err;
	bool wifiAvailable = false;
	bool updated = false;
	MojInt64 confidence = (MojInt64)ConnectionConfidenceUnknown;

	MojObject wifi;
	bool found = response.get(_T("wifi"), wifi);
	// if there is a wired connection, show it as a wifi one
	if(!found)
		found = response.get(_T("wired"), wifi);
	if (found) {
		updated = m_wifiRequirementCore->SetCurrentValue(wifi);

		MojString wifiConnected;
		found = false;
		err = wifi.get(_T("state"), wifiConnected, found);
		MojErrCheck(err);

		if (!found) {
			LOG_AM_WARNING(MSGID_WIFI_CONN_STATUS_UNKNOWN, 0, "Wifi connection status not returned by Connection Manager");
		} else if (wifiConnected == "connected") {
			MojString onInternet;
			err = wifi.get(_T("onInternet"), onInternet, found);
			MojErrCheck(err);

			if (onInternet == "yes") {
				wifiAvailable = true;
				confidence = GetConfidence(wifi);
			}
		}
	} else {
		LOG_AM_WARNING(MSGID_WIFI_STATUS_UNKNOWN, 0, "Wifi status not returned by Connection Manager");
	}

	if (wifiAvailable) {
		if (!m_wifiRequirementCore->IsMet()) {
			LOG_AM_DEBUG("Wifi connection is now available");
			m_wifiRequirementCore->Met();
			std::for_each(m_wifiRequirements.begin(), m_wifiRequirements.end(),
				boost::mem_fn(&Requirement::Met));
		} else if (updated) {
			std::for_each(m_wifiRequirements.begin(), m_wifiRequirements.end(),
				boost::mem_fn(&Requirement::Updated));
		}
	} else {
		if (m_wifiRequirementCore->IsMet()) {
			LOG_AM_DEBUG("Wifi connection is no longer available");
			m_wifiRequirementCore->Unmet();
			std::for_each(m_wifiRequirements.begin(), m_wifiRequirements.end(),
				boost::mem_fn(&Requirement::Unmet));
		}
	}

	if (m_wifiConfidence != (int)confidence) {
		m_wifiConfidence = (int)confidence;
		LOG_AM_DEBUG("Wifi confidence level changed to %d",
			m_wifiConfidence);
		UpdateConfidenceRequirements(m_wifiConfidenceCores,
			m_wifiConfidenceRequirements, m_wifiConfidence);
	}

	return MojErrNone;
}

MojErr ConnectionManagerProxy::UpdateWANStatus(const MojObject& response)
{
	MojErr err;
	bool wanAvailable = false;
	bool updated = false;
	MojInt64 confidence = (MojInt64)ConnectionConfidenceUnknown;

	MojObject wan;
	bool found = response.get(_T("wan"), wan);
	// webos-connman-adapter now set a "cellular" property instead of "wan"
	if(!found)
		found = response.get(_T("cellular"), wan);
	if (found) {
		updated = m_wanRequirementCore->SetCurrentValue(wan);

		MojString wanConnected;
		found = false;
		err = wan.get(_T("state"), wanConnected, found);
		MojErrCheck(err);

		if (!found) {
			LOG_AM_WARNING(MSGID_WAN_CONN_STATUS_UNKNOWN, 0, "WAN connection status not returned by Connection Manager");
		} else if (wanConnected == "connected") {
			MojString wanNetwork;
			found = false;
			err = wan.get(_T("network"), wanNetwork, found);
			MojErrCheck(err);

			if (!found) {
				LOG_AM_WARNING(MSGID_WAN_NW_MODE_UNKNOWN, 0, "WAN network mode not returned by Connection Manager");
			} else if (wanNetwork != "unusable") {
				MojString onInternet;
				err = wan.get(_T("onInternet"), onInternet, found);
				MojErrCheck(err);

				if (onInternet == "yes") {
					wanAvailable = true;
					confidence = GetConfidence(wan);
				}
			}
		}
	}

	if (wanAvailable) {
		if (!m_wanRequirementCore->IsMet()) {
			LOG_AM_DEBUG("WAN connection is now available");
			m_wanRequirementCore->Met();
			std::for_each(m_wanRequirements.begin(), m_wanRequirements.end(),
				boost::mem_fn(&Requirement::Met));
		} else if (updated) {
			std::for_each(m_wanRequirements.begin(), m_wanRequirements.end(),
				boost::mem_fn(&Requirement::Updated));
		}
	} else {
		if (m_wanRequirementCore->IsMet()) {
			LOG_AM_DEBUG("WAN connection is no longer available");
			m_wanRequirementCore->Unmet();
			std::for_each(m_wanRequirements.begin(), m_wanRequirements.end(),
				boost::mem_fn(&Requirement::Unmet));
		}
	}

	if (m_wanConfidence != (int)confidence) {
		m_wanConfidence = (int)confidence;
		LOG_AM_DEBUG("WAN confidence level changed to %d",
			m_wanConfidence);
		UpdateConfidenceRequirements(m_wanConfidenceCores,
			m_wanConfidenceRequirements, m_wanConfidence);
	}

	return MojErrNone;
}

int ConnectionManagerProxy::GetConfidence(const MojObject& spec) const
{
	MojObject confidenceObj;

	bool found = spec.get(_T("networkConfidenceLevel"),
		confidenceObj);
	if (!found) {
		LOG_AM_WARNING(MSGID_GET_NW_CONFIDENCE_FAIL, 0,
			    "Failed to retreive network confidence from network description %s", MojoObjectJson(spec).c_str());
	} else {
		return ConfidenceToInt(confidenceObj);

	}

	return ConnectionConfidenceUnknown;
}

int ConnectionManagerProxy::ConfidenceToInt(const MojObject& confidenceObj) const
{
	if (confidenceObj.type() != MojObject::TypeString) {
		LOG_AM_WARNING(MSGID_NON_STRING_TYPE_NW_CONFIDENCE, 0, "Network confidence must be specified as a string");
		return ConnectionConfidenceUnknown;
	}

	MojString confidenceStr;
	MojErr err = confidenceObj.stringValue(confidenceStr);
	if (err != MojErrNone) {
		LOG_AM_WARNING(MSGID_GET_NW_CONFIDENCE_LEVEL_FAIL, 0, "Failed to retreive network confidence level as string");
		return ConnectionConfidenceUnknown;
	}

	for (int i = 0; i < ConnectionConfidenceMax; ++i) {
		if (confidenceStr == ConnectionConfidenceNames[i]) {
			return i;
		}
	}

	LOG_AM_DEBUG("Unknown connection confidence string: \"%s\"", confidenceStr.data() );

	return ConnectionConfidenceUnknown;
}

boost::shared_ptr<Requirement>
ConnectionManagerProxy::InstantiateConfidenceRequirement(
	boost::shared_ptr<Activity> activity,
	boost::shared_ptr<RequirementCore> *confidenceCores,
	RequirementList* confidenceLists, const MojObject& confidenceDesc)
{
	int confidence = ConfidenceToInt(confidenceDesc);

	if (confidence == ConnectionConfidenceUnknown) {
		throw std::runtime_error("Invalid connection confidence level "
			"specified");
	}

	if ((confidence < 0) || (confidence >= ConnectionConfidenceMax)) {
		throw std::runtime_error("Confidence out of range");
	}

	boost::shared_ptr<ListedRequirement> req =
		boost::make_shared<BasicCoreListedRequirement>(
			activity, confidenceCores[confidence],
			confidenceCores[confidence]->IsMet());
	confidenceLists[confidence].push_back(*req);
	return req;
}

void ConnectionManagerProxy::UpdateConfidenceRequirements(
	boost::shared_ptr<RequirementCore> *confidenceCores,
	RequirementList *confidenceLists, int confidence)
{
	if (((confidence < 0) || (confidence >= ConnectionConfidenceMax)) &&
		(confidence != ConnectionConfidenceUnknown)) {
		LOG_AM_WARNING(MSGID_UNKNOWN_CONN_CONFIDENCE_LEVEL, 1, PMLOGKFV("conn_confidence_level","%d",confidence),
			    "Unknown connection confidence level seen attempting to update confidence requirements");
		return;
	}

	MojString& confidenceName = (confidence == ConnectionConfidenceUnknown) ?
		ConnectionConfidenceUnknownName :
		ConnectionConfidenceNames[confidence];

	for (int i = 0; i < ConnectionConfidenceMax; ++i) {
		confidenceCores[i]->SetCurrentValue(MojObject(confidenceName));

		if (confidence < i) {
			if (confidenceCores[i]->IsMet()) {
				confidenceCores[i]->Unmet();
				std::for_each(confidenceLists[i].begin(),
					confidenceLists[i].end(),
					boost::mem_fn(&Requirement::Unmet));
			} else {
				std::for_each(confidenceLists[i].begin(),
					confidenceLists[i].end(),
					boost::mem_fn(&Requirement::Updated));
			}
		} else {
			if (!confidenceCores[i]->IsMet()) {
				confidenceCores[i]->Met();
				std::for_each(confidenceLists[i].begin(),
					confidenceLists[i].end(),
					boost::mem_fn(&Requirement::Met));
			} else {
				std::for_each(confidenceLists[i].begin(),
					confidenceLists[i].end(),
					boost::mem_fn(&Requirement::Updated));
			}
		}
	}
}

