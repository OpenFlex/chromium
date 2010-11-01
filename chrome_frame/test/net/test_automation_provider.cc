// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome_frame/test/net/test_automation_provider.h"

#include "base/command_line.h"
#include "base/file_version_info.h"
#include "base/path_service.h"
#include "chrome/test/automation/automation_messages.h"
#include "chrome_frame/test/net/test_automation_resource_message_filter.h"

namespace {

// A special command line switch to just run the unit tests without CF in
// the picture.  Can be useful when the harness itself needs to be debugged.
const wchar_t kNoCfTestRun[] = L"no-cf-test-run";

bool CFTestsDisabled() {
  static bool switch_present = CommandLine::ForCurrentProcess()->
      HasSwitch(kNoCfTestRun);
  return switch_present;
}

}  // end namespace

TestAutomationProvider* TestAutomationProvider::g_provider_instance_ = NULL;

TestAutomationProvider::TestAutomationProvider(
    Profile* profile,
    TestAutomationProviderDelegate* delegate)
    : AutomationProvider(profile), tab_handle_(-1), delegate_(delegate) {
  // We need to register the protocol factory before the
  // AutomationResourceMessageFilter registers the automation job factory to
  // ensure that we don't inadvarently end up handling http requests which
  // we don't expect. The initial chrome frame page for the network tests
  // issues http requests which our test factory should not handle.
  URLRequest::RegisterProtocolFactory("http",
                                      TestAutomationProvider::Factory);
  URLRequest::RegisterProtocolFactory("https",
                                      TestAutomationProvider::Factory);
  automation_resource_message_filter_ =
      new TestAutomationResourceMessageFilter(this);
  g_provider_instance_ = this;
}

TestAutomationProvider::~TestAutomationProvider() {
  g_provider_instance_ = NULL;
}

void TestAutomationProvider::OnMessageReceived(const IPC::Message& msg) {
  if (automation_resource_message_filter_->OnMessageReceived(msg))
    return;  // Message handled by the filter.

  __super::OnMessageReceived(msg);
}

// IPC override to grab the tab handle.
bool TestAutomationProvider::Send(IPC::Message* msg) {
  if (msg->type() == AutomationMsg_TabLoaded::ID) {
    DCHECK(tab_handle_ == -1) << "Currently only support one tab";
    void* iter = NULL;
    CHECK(msg->ReadInt(&iter, &tab_handle_));
    DVLOG(1) << "Got tab handle: " << tab_handle_;
    DCHECK(tab_handle_ != -1 && tab_handle_ != 0);
    delegate_->OnInitialTabLoaded();
  }

  return AutomationProvider::Send(msg);
}

URLRequestJob* TestAutomationProvider::Factory(URLRequest* request,
                                               const std::string& scheme) {
  if (CFTestsDisabled())
    return NULL;

  if (request->url().SchemeIs("http") || request->url().SchemeIs("https")) {
    // Only look at requests that don't have any user data.
    // ResourceDispatcherHost uses the user data for requests that it manages.
    // We don't want to mess with those.

    // We could also check if the current thread is our TestUrlRequest thread
    // and only intercept requests that belong to that thread.
    if (g_provider_instance_ && request->GetUserData(NULL) == NULL &&
        g_provider_instance_->tab_handle_ != -1) {
      // We generate our own request id which is also what
      // ResourceDispatcherHost does (well, the id is actually generated by
      // ResourceDispatcher).  Since these requests are divided into with
      // and without userdata, we're OK.  However, just to make debugging
      // a little easier, we have a significantly higher start value.
      static int new_id = 0x00100000;
      URLRequestAutomationJob* job = new URLRequestAutomationJob(request,
          g_provider_instance_->tab_handle_, new_id++,
          g_provider_instance_->automation_resource_message_filter_, false);
      return job;
    }
  }

  return NULL;
}

std::string TestAutomationProvider::GetProtocolVersion() {
  // Return the version of chrome.dll
  FilePath path;
  PathService::Get(base::DIR_MODULE, &path);
  path = path.AppendASCII("chrome.dll");

  std::string version;
  scoped_ptr<FileVersionInfo> version_info(
      FileVersionInfo::CreateFileVersionInfo(path));
  if (version_info.get()) {
    version = WideToASCII(version_info->product_version());
  }
  return version;
}

// static
TestAutomationProvider* TestAutomationProvider::NewAutomationProvider(
    Profile* p, const std::string& channel,
    TestAutomationProviderDelegate* delegate) {
  TestAutomationProvider* automation = new TestAutomationProvider(p, delegate);
  automation->ConnectToChannel(channel);
  automation->SetExpectedTabCount(1);
  return automation;
}
