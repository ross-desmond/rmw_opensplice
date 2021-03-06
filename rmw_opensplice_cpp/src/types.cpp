// Copyright 2014-2015 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "types.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rmw/error_handling.h"
#include "rosidl_typesupport_opensplice_cpp/misc.hpp"
#include "demangle.hpp"
#include "namespace_prefix.hpp"

std::string
create_type_name(
  const message_type_support_callbacks_t * callbacks,
  const std::string & sep)
{
  return std::string(callbacks->package_name) +
         "::" + sep + "::dds_::" + callbacks->message_name + "_";
}

CustomDataReaderListener::CustomDataReaderListener()
: print_discovery_logging_(false)
{
  // TODO(wjwwood): replace this with actual console logging when we have that
  char * discovery_logging_value = nullptr;
  const char * discovery_logging_env = "RMW_PRINT_DISCOVERY_LOGGING";
  size_t discovery_logging_size;
#ifndef _WIN32
  discovery_logging_value = getenv(discovery_logging_env);
  if (discovery_logging_value) {
    discovery_logging_size = strlen(discovery_logging_value);
  }
#else
  _dupenv_s(&discovery_logging_value, &discovery_logging_size, discovery_logging_env);
#endif
  if (discovery_logging_value) {
    std::string str(discovery_logging_value, discovery_logging_size);
    std::string str_lower(str);
    std::transform(
      str_lower.begin(), str_lower.end(), str_lower.begin(),
      [](unsigned char c) {
        return std::tolower(c);
      });
    if (str != "0" && str_lower != "false" && str_lower != "off") {
      print_discovery_logging_ = true;
    }
#ifdef _WIN32
    free(discovery_logging_value);
#endif
  }
}

size_t
CustomDataReaderListener::count_topic(const char * topic_name)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(
    topic_cache.getTopicToTypes().begin(),
    topic_cache.getTopicToTypes().end(),
    [&](auto tnt) -> bool {
      auto fqdn = _demangle_if_ros_topic(tnt.first);
      if (fqdn == topic_name) {
        return true;
      }
      return false;
    });
  size_t count;
  if (it == topic_cache.getTopicToTypes().end()) {
    count = 0;
  } else {
    count = it->second.size();
  }
  return count;
}

void
CustomDataReaderListener::fill_topic_names_and_types(
  bool no_demangle,
  std::map<std::string, std::set<std::string>> & tnat)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it : topic_cache.getTopicToTypes()) {
    if (!no_demangle && (_get_ros_prefix_if_exists(it.first) !=
      rosidl_typesupport_opensplice_cpp::get_ros_topic_prefix()))
    {
      continue;
    }
    for (auto & jt : it.second) {
      tnat[it.first].insert(jt);
    }
  }
}

void
CustomDataReaderListener::fill_service_names_and_types(
  std::map<std::string, std::set<std::string>> & services)
{
  for (auto it : topic_cache.getTopicToTypes()) {
    std::string service_name = _demangle_service_from_topic(it.first);
    if (!service_name.length()) {
      // not a service
      continue;
    }
    for (auto & itt : it.second) {
      std::string service_type = _demangle_service_type_only(itt);
      if (service_type.length()) {
        services[service_name].insert(service_type);
      }
    }
  }
}

void CustomDataReaderListener::fill_topic_names_and_types_by_guid(
    bool no_demangle,
    std::map<std::string, std::set<std::string>> & tnat,
    GuidPrefix_t &guid) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& map = topic_cache.getParticipantToTopics().find(guid);
  if (map == topic_cache.getParticipantToTopics().end()) {
    return;
  }
  for (auto& it : map->second) {
    if (!no_demangle && (_get_ros_prefix_if_exists(it.first) !=
                         rosidl_typesupport_opensplice_cpp::get_ros_topic_prefix()))
    {
      continue;
    }
    for (auto & jt : it.second) {
      tnat[it.first].insert(jt);
    }
  }
}

void CustomDataReaderListener::fill_service_names_and_types_by_guid(
    std::map<std::string, std::set<std::string>> & services,
    GuidPrefix_t &guid) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& map = topic_cache.getParticipantToTopics().find(guid);
  if (map == topic_cache.getParticipantToTopics().end()) {
    return;
  }
  for (auto& it : map->second) {
    std::string service_name = _demangle_service_from_topic(it.first);
    if (!service_name.length()) {
      // not a service
      continue;
    }
    for (auto & itt : it.second) {
      std::string service_type = _demangle_service_type_only(itt);
      if (service_type.length()) {
        services[service_name].insert(service_type);
      }
    }
  }
}

void
print_discovery_logging(
  const std::string & prefix,
  const std::string & name,
  const std::string & type,
  CustomDataReaderListener::EndPointType end_point_type)
{
  // filter builtin OpenSplice topics
  if (
    name.compare(0, 4, "DCPS") != 0 &&
    name.compare(0, 2, "d_") != 0 &&
    name.compare(0, 2, "q_") != 0 &&
    name.compare("CMParticipant") != 0)
  {
    std::string msg = prefix +
      ((end_point_type == CustomDataReaderListener::PublisherEP) ? "P" : "S") +
      ": " + name + " <" + type + ">";
    printf("%s\n", msg.c_str());
  }
}

CustomPublisherListener::CustomPublisherListener(rmw_guard_condition_t * graph_guard_condition)
: graph_guard_condition_(graph_guard_condition)
{
}

void
CustomPublisherListener::on_data_available(DDS::DataReader * reader)
{
  std::lock_guard<std::mutex> lock(mutex_);
  DDS::PublicationBuiltinTopicDataDataReader * builtin_reader =
    DDS::PublicationBuiltinTopicDataDataReader::_narrow(reader);

  DDS::PublicationBuiltinTopicDataSeq data_seq;

  DDS::SampleInfoSeq info_seq;
  DDS::ReturnCode_t retcode = builtin_reader->take(
    data_seq, info_seq, DDS::LENGTH_UNLIMITED,
    DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);

  if (retcode == DDS::RETCODE_NO_DATA) {
    return;
  }
  if (retcode != DDS::RETCODE_OK) {
    fprintf(stderr, "failed to access data from the built-in reader\n");
    return;
  }

  for (DDS::ULong i = 0; i < data_seq.length(); ++i) {
    std::string topic_name = "";
    if (info_seq[i].valid_data) {
      GuidPrefix_t guid;
      DDS_BuiltinTopicKey_to_GUID(&guid, data_seq[i].key);
      if (info_seq[i].instance_state == DDS::ALIVE_INSTANCE_STATE) {
        std::lock_guard<std::mutex> lk(mutex_);
        topic_name = data_seq[i].topic_name.in();
        topic_cache.addTopic(guid, topic_name, data_seq[i].type_name.in());
        print_discovery_logging("+", topic_name, data_seq[i].type_name.in(), PublisherEP);
      } else {
        topic_cache.removeTopic(guid, topic_name, data_seq[i].type_name.in());
      }
    } else {
      //TODO: handle this case, is this allowed if the data is not valid?
    }
  }

  if (data_seq.length() > 0) {
    rmw_ret_t ret = rmw_trigger_guard_condition(graph_guard_condition_);
    if (ret != RMW_RET_OK) {
      fprintf(stderr, "failed to trigger graph guard condition: %s\n", rmw_get_error_string().str);
    }
  }

  builtin_reader->return_loan(data_seq, info_seq);
}

CustomSubscriberListener::CustomSubscriberListener(rmw_guard_condition_t * graph_guard_condition)
: graph_guard_condition_(graph_guard_condition)
{
}

void
CustomSubscriberListener::on_data_available(DDS::DataReader * reader)
{
  std::lock_guard<std::mutex> lock(mutex_);
  DDS::SubscriptionBuiltinTopicDataDataReader * builtin_reader =
    DDS::SubscriptionBuiltinTopicDataDataReader::_narrow(reader);

  DDS::SubscriptionBuiltinTopicDataSeq data_seq;
  DDS::SampleInfoSeq info_seq;

  DDS::ReturnCode_t retcode = builtin_reader->take(
    data_seq, info_seq, DDS::LENGTH_UNLIMITED,
    DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ANY_INSTANCE_STATE);

  if (retcode == DDS::RETCODE_NO_DATA) {
    return;
  }
  if (retcode != DDS::RETCODE_OK) {
    fprintf(stderr, "failed to access data from the built-in reader\n");
    return;
  }

  for (DDS::ULong i = 0; i < data_seq.length(); ++i) {
    std::string topic_name = "";
    if (info_seq[i].valid_data) {
        GuidPrefix_t guid;
        DDS_BuiltinTopicKey_to_GUID(&guid, data_seq[i].key);
        std::lock_guard<std::mutex> lk(mutex_);
        if (info_seq[i].instance_state == DDS::ALIVE_INSTANCE_STATE) {
            topic_name = data_seq[i].topic_name.in();
            topic_cache.addTopic(guid, topic_name, data_seq[i].type_name.in());
            print_discovery_logging("+", topic_name, data_seq[i].type_name.in(), SubscriberEP);
        } else {
            topic_cache.removeTopic(guid, topic_name, data_seq[i].type_name.in());
        }
    } else {
        //TODO: handle this case, is this allowed if the data is not valid?
    }
  }

  if (data_seq.length() > 0) {
    rmw_ret_t ret = rmw_trigger_guard_condition(graph_guard_condition_);
    if (ret != RMW_RET_OK) {
      fprintf(stderr, "failed to trigger graph guard condition: %s\n", rmw_get_error_string().str);
    }
  }

  builtin_reader->return_loan(data_seq, info_seq);
}

void OpenSplicePublisherListener::on_publication_matched(
  DDS::DataWriter_ptr writer,
  const DDS::PublicationMatchedStatus & status)
{
  (void) writer;
  current_count_ = status.current_count;
}

size_t OpenSplicePublisherListener::current_count() const
{
  return current_count_;
}

void OpenSpliceSubscriberListener::on_subscription_matched(
  DDS::DataReader_ptr reader,
  const DDS::SubscriptionMatchedStatus & status)
{
  (void) reader;
  current_count_ = status.current_count;
}

size_t OpenSpliceSubscriberListener::current_count() const
{
  return current_count_;
}
