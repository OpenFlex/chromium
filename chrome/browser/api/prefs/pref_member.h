// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A helper class that stays in sync with a preference (bool, int, real,
// string or filepath).  For example:
//
// class MyClass {
//  public:
//   MyClass(PrefService* prefs) {
//     my_string_.Init(prefs::kHomePage, prefs);
//   }
//  private:
//   StringPrefMember my_string_;
// };
//
// my_string_ should stay in sync with the prefs::kHomePage pref and will
// update if either the pref changes or if my_string_.SetValue is called.
//
// An optional observer can be passed into the Init method which can be used to
// notify MyClass of changes. Note that if you use SetValue(), the observer
// will not be notified.

#ifndef CHROME_BROWSER_API_PREFS_PREF_MEMBER_H_
#define CHROME_BROWSER_API_PREFS_PREF_MEMBER_H_

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/file_path.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/message_loop_proxy.h"
#include "base/prefs/public/pref_observer.h"
#include "base/values.h"

class PrefServiceBase;

namespace subtle {

class PrefMemberBase : public PrefObserver {
 public:
  // Type of callback you can register if you need to know the name of
  // the pref that is changing.
  typedef base::Callback<void(const std::string&)> NamedChangeCallback;

  PrefServiceBase* prefs() { return prefs_; }
  const PrefServiceBase* prefs() const { return prefs_; }

 protected:
  class Internal : public base::RefCountedThreadSafe<Internal> {
   public:
    Internal();

    // Update the value, either by calling |UpdateValueInternal| directly
    // or by dispatching to the right thread.
    // Takes ownership of |value|.
    void UpdateValue(base::Value* value,
                     bool is_managed,
                     bool is_user_modifiable,
                     const base::Closure& callback) const;

    void MoveToThread(
        const scoped_refptr<base::MessageLoopProxy>& message_loop);

    // See PrefMember<> for description.
    bool IsManaged() const {
      return is_managed_;
    }

    bool IsUserModifiable() const {
      return is_user_modifiable_;
    }

   protected:
    friend class base::RefCountedThreadSafe<Internal>;
    virtual ~Internal();

    void CheckOnCorrectThread() const {
      DCHECK(IsOnCorrectThread());
    }

   private:
    // This method actually updates the value. It should only be called from
    // the thread the PrefMember is on.
    virtual bool UpdateValueInternal(const base::Value& value) const = 0;

    bool IsOnCorrectThread() const;

    scoped_refptr<base::MessageLoopProxy> thread_loop_;
    mutable bool is_managed_;
    mutable bool is_user_modifiable_;

    DISALLOW_COPY_AND_ASSIGN(Internal);
  };

  PrefMemberBase();
  virtual ~PrefMemberBase();

  // See PrefMember<> for description.
  void Init(const char* pref_name, PrefServiceBase* prefs,
            const NamedChangeCallback& observer);
  void Init(const char* pref_name, PrefServiceBase* prefs);

  virtual void CreateInternal() const = 0;

  // See PrefMember<> for description.
  void Destroy();

  void MoveToThread(const scoped_refptr<base::MessageLoopProxy>& message_loop);

  // PrefObserver
  virtual void OnPreferenceChanged(PrefServiceBase* service,
                                   const std::string& pref_name) OVERRIDE;

  void VerifyValuePrefName() const {
    DCHECK(!pref_name_.empty());
  }

  // This method is used to do the actual sync with the preference.
  // Note: it is logically const, because it doesn't modify the state
  // seen by the outside world. It is just doing a lazy load behind the scenes.
  void UpdateValueFromPref(const base::Closure& callback) const;

  // Verifies the preference name, and lazily loads the preference value if
  // it hasn't been loaded yet.
  void VerifyPref() const;

  const std::string& pref_name() const { return pref_name_; }

  virtual Internal* internal() const = 0;

  // Used to allow registering plain base::Closure callbacks.
  static void InvokeUnnamedCallback(const base::Closure& callback,
                                    const std::string& pref_name);

 private:
  // Ordered the members to compact the class instance.
  std::string pref_name_;
  NamedChangeCallback observer_;
  PrefServiceBase* prefs_;

 protected:
  bool setting_value_;
};

// This function implements StringListPrefMember::UpdateValue().
// It is exposed here for testing purposes.
bool PrefMemberVectorStringUpdate(const Value& value,
                                  std::vector<std::string>* string_vector);

}  // namespace subtle

template <typename ValueType>
class PrefMember : public subtle::PrefMemberBase {
 public:
  // Defer initialization to an Init method so it's easy to make this class be
  // a member variable.
  PrefMember() {}
  virtual ~PrefMember() {}

  // Do the actual initialization of the class.  Use the two-parameter
  // version if you don't want any notifications of changes.  This
  // method should only be called on the UI thread.
  void Init(const char* pref_name, PrefServiceBase* prefs,
            const NamedChangeCallback& observer) {
    subtle::PrefMemberBase::Init(pref_name, prefs, observer);
  }
  void Init(const char* pref_name, PrefServiceBase* prefs,
            const base::Closure& observer) {
    subtle::PrefMemberBase::Init(
        pref_name, prefs,
        base::Bind(&PrefMemberBase::InvokeUnnamedCallback, observer));
  }
  void Init(const char* pref_name, PrefServiceBase* prefs) {
    subtle::PrefMemberBase::Init(pref_name, prefs);
  }

  // Unsubscribes the PrefMember from the PrefService. After calling this
  // function, the PrefMember may not be used any more on the UI thread.
  // Assuming |MoveToThread| was previously called, |GetValue|, |IsManaged|,
  // and |IsUserModifiable| can still be called from the other thread but
  // the results will no longer update from the PrefService.
  // This method should only be called on the UI thread.
  void Destroy() {
    subtle::PrefMemberBase::Destroy();
  }

  // Moves the PrefMember to another thread, allowing read accesses from there.
  // Changes from the PrefService will be propagated asynchronously
  // via PostTask.
  // This method should only be used from the thread the PrefMember is currently
  // on, which is the UI thread by default.
  void MoveToThread(const scoped_refptr<base::MessageLoopProxy>& message_loop) {
    subtle::PrefMemberBase::MoveToThread(message_loop);
  }

  // Check whether the pref is managed, i.e. controlled externally through
  // enterprise configuration management (e.g. windows group policy). Returns
  // false for unknown prefs.
  // This method should only be used from the thread the PrefMember is currently
  // on, which is the UI thread unless changed by |MoveToThread|.
  bool IsManaged() const {
    VerifyPref();
    return internal_->IsManaged();
  }

  // Checks whether the pref can be modified by the user. This returns false
  // when the pref is managed by a policy or an extension, and when a command
  // line flag overrides the pref.
  // This method should only be used from the thread the PrefMember is currently
  // on, which is the UI thread unless changed by |MoveToThread|.
  bool IsUserModifiable() const {
    VerifyPref();
    return internal_->IsUserModifiable();
  }

  // Retrieve the value of the member variable.
  // This method should only be used from the thread the PrefMember is currently
  // on, which is the UI thread unless changed by |MoveToThread|.
  ValueType GetValue() const {
    VerifyPref();
    return internal_->value();
  }

  // Provided as a convenience.
  ValueType operator*() const {
    return GetValue();
  }

  // Set the value of the member variable.
  // This method should only be called on the UI thread.
  void SetValue(const ValueType& value) {
    VerifyValuePrefName();
    setting_value_ = true;
    UpdatePref(value);
    setting_value_ = false;
  }

  // Returns the pref name.
  const std::string& GetPrefName() const {
    return pref_name();
  }

 private:
  class Internal : public subtle::PrefMemberBase::Internal {
   public:
    Internal() : value_(ValueType()) {}

    ValueType value() {
      CheckOnCorrectThread();
      return value_;
    }

   protected:
    virtual ~Internal() {}

    virtual bool UpdateValueInternal(const base::Value& value) const OVERRIDE;

    // We cache the value of the pref so we don't have to keep walking the pref
    // tree.
    mutable ValueType value_;

    DISALLOW_COPY_AND_ASSIGN(Internal);
  };

  virtual Internal* internal() const OVERRIDE { return internal_; }
  virtual void CreateInternal() const OVERRIDE {
    internal_ = new Internal();
  }

  // This method is used to do the actual sync with pref of the specified type.
  void UpdatePref(const ValueType& value);

  mutable scoped_refptr<Internal> internal_;

  DISALLOW_COPY_AND_ASSIGN(PrefMember);
};

typedef PrefMember<bool> BooleanPrefMember;
typedef PrefMember<int> IntegerPrefMember;
typedef PrefMember<double> DoublePrefMember;
typedef PrefMember<std::string> StringPrefMember;
typedef PrefMember<FilePath> FilePathPrefMember;
// This preference member is expensive for large string arrays.
typedef PrefMember<std::vector<std::string> > StringListPrefMember;

#endif  // CHROME_BROWSER_API_PREFS_PREF_MEMBER_H_
