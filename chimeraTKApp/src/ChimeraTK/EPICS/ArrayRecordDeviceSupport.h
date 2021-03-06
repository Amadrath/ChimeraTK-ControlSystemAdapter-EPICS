/*
 * ChimeraTK control-system adapter for EPICS.
 *
 * Copyright 2015-2019 aquenos GmbH
 *
 * The ChimeraTK Control System Adapter for EPICS is free software: you can
 * redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License version 3 as published by the Free Software Foundation.
 *
 * The ChimeraTK Control System Adapter for EPICS is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the ChimeraTK Control System Adapter for EPICS. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef CHIMERATK_EPICS_ARRAY_RECORD_DEVICE_SUPPORT_H
#define CHIMERATK_EPICS_ARRAY_RECORD_DEVICE_SUPPORT_H

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
#include <utility>

extern "C" {
#include <callback.h>
#include <dbFldTypes.h>
#include <dbLink.h>
#include <dbScan.h>
#include <epicsTypes.h>
} // extern "C"

#include "RecordDeviceSupportBase.h"
#include "RecordDirection.h"
#include "RecordValueFieldName.h"
#include "ensureScanIoRequest.h"

namespace ChimeraTK {
namespace EPICS {

namespace detail {

/**
 * Helper structure for reading and writing data from and to the record's value
 * buffer. This structure also provides a function for initializing the buffer
 * that stores the record's value.
 *
 * This structure is used so that we can implement a different logic for dealing
 * with an array of strings, where we cannot simply copy a block of memory.
 *
 * This data-structure does not update the record's NORD field when writing
 * data.
 *
 * The first template parameter is the record type and the second template
 * parameter is the type of the value's elements (e.g. int, string, etc.).
 */
template<typename RecordType, typename T>
struct ArrayRecordBufferHelper {

  inline static void initializeBuffer(RecordType *record) {
    record->bptr = new T[record->nelm];
    // Ensure that the buffer is clean.
    std::memset(record->bptr, 0, sizeof(T) * record->nelm);
  }

  inline static std::vector<T> readValue(RecordType *record) {
    std::vector<T> value(record->nelm);
    std::memcpy(
      value.data(),
      record->bptr,
      record->nelm * sizeof(T));
    return value;
  }

  inline static void writeValue(RecordType *record, std::vector<T> const &value) {
    std::memcpy(
        record->bptr,
        value.data(),
        record->nelm * sizeof(T));
  }

};

template<typename RecordType>
struct ArrayRecordBufferHelper<RecordType, std::string> {

  inline static void initializeBuffer(RecordType *record) {
    record->bptr = new char[MAX_STRING_SIZE * record->nelm];
    std::memset(record->bptr, 0, MAX_STRING_SIZE * record->nelm);
  }

  inline static std::vector<std::string> readValue(RecordType *record) {
    std::vector<std::string> value(record->nelm);
    // The EPICS Base code ensure that all strings are null-terminated. The
    // relevant code can be found in dbConvert.c and dbFastLinkConv.c.
    // However, if NORD < NELM, the rest of the array might contain garbage, so
    // we ensure that any extra elements are empty strings.
    char *buffer = static_cast<char *>(record->bptr);
    for (auto i = record->nord; i < record->nelm; ++i) {
      buffer[i * MAX_STRING_SIZE] = '\0';
    }
    for (std::size_t i = 0; i < record->nelm; ++i) {
      // There is an assignment operator that expects a pointer to a
      // null-terminated string, so that the following line is perfectly legal.
      value[i] = buffer + i * MAX_STRING_SIZE;
    }
    return value;
  }

  inline static void writeValue(RecordType *record, std::vector<std::string> const &value) {
    char *buffer = static_cast<char *>(record->bptr);
    for (std::size_t i = 0; i < record->nelm; ++i) {
      auto element = buffer + i * MAX_STRING_SIZE;
      std::strncpy(element, value[i].c_str(), MAX_STRING_SIZE);
      // We have to ensure that the string is null-terminated, even if it
      // exceeds the size allowed by EPICS.
      element[MAX_STRING_SIZE - 1] = '\0';
    }
  }

};

/**
 * Base class for ArrayRecordDeviceSupport. This class implements the code
 * that is shared by the device supports for input and output records.
 *
 * The template parameter is the record's data-structure type.
 */
template<typename RecordType>
class ArrayRecordDeviceSupportTrait : public RecordDeviceSupportBase {

public:

  /**
   * Constructor. Takes a pointer to the record's data-structure and a reference
   * to the INP or OUT field.
   */
  ArrayRecordDeviceSupportTrait(
      RecordType *record, ::DBLINK const &linkField)
      : RecordDeviceSupportBase(RecordAddress::parse(linkField)),
        record(record) {
    // TODO Allow a mismatch between the type and the FTVL and use a
    // ConvertingPVSupport in this case.
    if (this->valueType == typeid(std::int8_t)) {
      checkFtvl(DBF_CHAR);
    } else if (this->valueType == typeid(std::uint8_t)) {
      checkFtvl(DBF_CHAR);
    } else if (this->valueType == typeid(std::int16_t)) {
      checkFtvl(DBF_SHORT);
    } else if (this->valueType == typeid(std::uint16_t)) {
      checkFtvl(DBF_SHORT);
    } else if (this->valueType == typeid(std::int32_t)) {
      checkFtvl(DBF_LONG);
    } else if (this->valueType == typeid(std::uint32_t)) {
      checkFtvl(DBF_LONG);
    } else if (this->valueType == typeid(float)) {
      checkFtvl(DBF_FLOAT);
    } else if (this->valueType == typeid(double)) {
      checkFtvl(DBF_DOUBLE);
    } else if (this->valueType == typeid(std::string)) {
      checkFtvl(DBF_STRING);
    } else {
      throw std::logic_error(
        std::string("Unexpected value type: ") + valueType.name());
    }
    // The ChimeraTK Control System Adapter and ChimeraTK Device Access ensure
    // that the number of elements does not change after initialization. For
    // this reason, it is sufficient when we check the number of elements once
    // during initialization.
    std::size_t numberOfElements = this->pvSupport->getNumberOfElements();
    if (numberOfElements != record->nelm) {
      std::ostringstream oss;
      oss << "Process variable has " << numberOfElements
          << " elements, but the record's NELM field specifies " << record->nelm
          << " elements.";
      throw std::invalid_argument(oss.str());
    }
  }

protected:

  /**
   * Auxilliary data strucuture needed by callbackRequestProcessCallback.
   */
  ::CALLBACK processCallback;

  /**
   * Pointer to the record structure.
   */
  RecordType *record;

  /**
   * Updates the record's TIME field with the specified time stamp.
   */
  void updateTimeStamp(VersionNumber const &versionNumber) {
    auto time = versionNumber.getTime();
    std::chrono::nanoseconds::rep timeInNanosecs = std::chrono::time_point_cast<std::chrono::nanoseconds>(time).
                                                   time_since_epoch().count();
    std::chrono::seconds::rep secs = timeInNanosecs / 1000000000;
    record->time.secPastEpoch =
        (secs < POSIX_TIME_AT_EPICS_EPOCH) ?
            0 : (secs - POSIX_TIME_AT_EPICS_EPOCH);
    record->time.nsec = timeInNanosecs % 1000000000;
  }

private:

  /**
   * Checks that the record's FTVL field has the specified value. Throws an
   * exception if the value of the FTVL field is different.
   */
  void checkFtvl(::epicsEnum16 expectedFtvl) {
    if (this->record->ftvl != expectedFtvl) {
      throw std::invalid_argument(
        std::string("Invalid FTVL for PV value type '")
        + this->valueType.name()
        + "'. Please make sure that the type specified in the record's FTVL field matches the element type of the PV.");
    }
  }

};

} // namespace detail

/**
 * Record device support class for array records.
 *
 * The template parameters are the record's data-structure type and the direction
 * of the record (input or output).
 */
template<typename RecordType, RecordDirection Direction = detectRecordDirection<RecordType>()>
class ArrayRecordDeviceSupport;

// Template specialization for input records.
template<typename RecordType>
class ArrayRecordDeviceSupport<RecordType, RecordDirection::INPUT>
    : public detail::ArrayRecordDeviceSupportTrait<RecordType> {

public:

  /**
   * Constructor. Takes a pointer to the record's data-structure.
   */
  ArrayRecordDeviceSupport(RecordType *record)
      : detail::ArrayRecordDeviceSupportTrait<RecordType>(
          record, record->inp),
        ioIntrModeEnabled(false) {
    // Prepare the data structure needed when enabling I/O Intr mode.
    ::scanIoInit(&this->ioIntrModeScanPvt);
  }

  /**
   * Processes a request to enable or disable the I/O Intr mode.
   */
  void getInterruptInfo(int command, ::IOSCANPVT *iopvt) {
    this->template callForValueType<CallGetInterruptInfoInternal>(
      this, command, iopvt);
  }

  /**
   * Starts a read operation, completes a read operation (depending on the
   * current state). When the record is in I/O Intr mode, this method is also
   * called to process a value that has been made available by the notify
   * callback.
   */
  void process() {
    this->template callForValueType<CallProcessInternal>(this);
  }

private:

  /**
   * Helper template for calling the right instantiation of
   * getInterruptInfoInternal for the current value type.
   */
  template<typename T>
  struct CallGetInterruptInfoInternal {
    void operator()(
        ArrayRecordDeviceSupport *obj, int command, ::IOSCANPVT *iopvt) {
      obj->template getInterruptInfoInternal<T>(command, iopvt);
    }
  };

  /**
   * Helper template for calling the right instantiation of processInternal for
   * the current value type.
   */
  template<typename T>
  struct CallProcessInternal {
    void operator()(ArrayRecordDeviceSupport *obj) {
      obj->template processInternal<T>();
    }
  };

  /**
   * Flag indicating whether the record has been set to I/O Intr mode.
   */
  bool ioIntrModeEnabled;

  /**
   * Datastructure internally needed by EPICS to handle the I/O Intr mode.
   * This data structure is initialized using scanIoInit() and later used
   * by getInterruptInfo(...).
   */
  ::IOSCANPVT ioIntrModeScanPvt;

  /**
   * Exception that was sent with last notification.
   */
  std::exception_ptr notifyException;

  /**
   * Version number / time stamp that belongs to notifyValue.
   */
  VersionNumber notifyVersionNumber;

  /**
   * Value that was sent with last notification. This is actually a pointer to a
   * const vector, but we only know the element type at runtime, so we have to
   * use a pointer to void here.
   */
  std::shared_ptr<void const> notifyValue;

  /**
   * Exception that happened during last read attempt.
   */
  std::exception_ptr readException;

  /**
   * Version number / Time stamp that belongs to readValue.
   */
  VersionNumber readVersionNumber;

  /**
   * Value that was read by last read attempt. This is actually a pointer to a
   * const vector, but we only know the element type at runtime, so we have to
   * use a pointer to void here.
   */
  std::shared_ptr<void const> readValue;

  /**
   * Internal implementation of getInterruptInfo(...). This is a template that
   * is instantiated for each supported element type. The calling method ensures
   * that the version for the current element type is called.
   */
  template<typename T>
  void getInterruptInfoInternal(int command, ::IOSCANPVT *iopvt) {
    auto pvSupport = this->template getPVSupport<T>();
    // A command value of 0 means enable I/O Intr mode, a value of 0 means
    // disable.
    if (command == 0) {
      if (!pvSupport->canNotify()) {
        throw std::runtime_error(
          "I/O Intr mode is not supported for this record.");
      }
      // We can safely pass this to the callback because a record device support
      // is never destroyed once successfully constructed.
      pvSupport->notify(
        [this](
            typename PVSupport<T>::SharedValue const &value,
            VersionNumber const &versionNumber) {
          this->notifyValue = value;
          this->notifyVersionNumber = versionNumber;
          ensureScanIoRequest(this->ioIntrModeScanPvt);
        },
        [this](std::exception_ptr const& error){
          this->notifyException = error;
          ensureScanIoRequest(this->ioIntrModeScanPvt);
        });
      this->ioIntrModeEnabled = true;
    } else {
      pvSupport->cancelNotify();
      this->ioIntrModeEnabled = false;
    }
    *iopvt = this->ioIntrModeScanPvt;
  }

  /**
   * Internal implementation of process(...). This is a template that is
   * instantiated for each supported element type. The calling method ensures
   * that the version for the current element type is called.
   */
  template<typename T>
  void processInternal() {
    // The queue used by callbackRequestProcessCallback internally uses a lock.
    // This means that the changes that we make asynchronously are visible to
    // the thread calling this method at a later point in time. The same applies
    // to the I/O Intr mode: scanIoRequest internally uses a callback that also
    // uses a lock.
    // This only leaves two corner cases: Switching from regular mode to
    // I/O Intr mode and the other way round. In both cases, it could happen
    // that the notify callback overwrites the value previously written by the
    // read callback, but before the record is processed again.
    // We avoid this problem by using separate variables for the value and
    // exception written by a read callback and the value and exception written
    // by a notify callback.
    // The notify callback is not called again before we have acknowledged that
    // we are finished with processing, so we do not have to worry about a
    // second notification overwriting the value of an earlier notification
    // before processing of the earlier value has completed.

    auto pvSupport = this->template getPVSupport<T>();

    // If the record's PACT field is set, this method is called because an
    // asynchronous read completed.
    if (this->record->pact) {
      this->record->pact = false;
      if (this->readException) {
        auto tempException = this->readException;
        this->readException = std::exception_ptr();
        std::rethrow_exception(tempException);
      }
      auto value = std::static_pointer_cast<typename PVSupport<T>::Value const>(
        this->readValue);
      // We already checked the number of elements and the element type during
      // construction, so we do not expect that they mismatch. We still do this
      // check here just in case something in ChimeraTK changes and we suddenly
      // get vectors of different sizes.
      if (this->record->nelm != value->size()) {
        std::ostringstream oss;
        oss << "Unexpected got a vector of length " << value->size()
            << " where a vector of length " << this->record->nelm
            << " was expected.";
        throw std::runtime_error(oss.str());
      }
      detail::ArrayRecordBufferHelper<RecordType, T>::writeValue(
          this->record, *value);
      this->record->nord = this->record->nelm;
      this->updateTimeStamp(this->readVersionNumber);
      return;
    }

    // If the ioIntrModeEnabled flag is set, this method is called because our
    // notify callback requested the record to be processed.
    if (this->ioIntrModeEnabled) {
      if (this->notifyException) {
        auto tempException = this->notifyException;
        this->notifyException = std::exception_ptr();
        pvSupport->notifyFinished();
        std::rethrow_exception(tempException);
      }
      auto value = std::static_pointer_cast<typename PVSupport<T>::Value const>(
        this->notifyValue);
      // We already checked the number of elements and the element type during
      // construction, so we do not expect that they mismatch. We still do this
      // check here just in case something in ChimeraTK changes and we suddenly
      // get vectors of different sizes.
      if (this->record->nelm != value->size()) {
        std::ostringstream oss;
        oss << "Unexpected got a vector of length " << value->size()
            << " where a vector of length " << this->record->nelm
            << " was expected.";
        throw std::runtime_error(oss.str());
      }
      detail::ArrayRecordBufferHelper<RecordType, T>::writeValue(
          this->record, *value);
      this->record->nord = this->record->nelm;
      this->updateTimeStamp(this->notifyVersionNumber);
      pvSupport->notifyFinished();
      return;
    }

    // In all other cases, this method is called because a new value should be
    // read.
    // We can safely pass this to the callback because a record device support
    // is never destroyed once successfully constructed.
    bool immediate = pvSupport->read(
      [this](bool immediate,
          typename PVSupport<T>::SharedValue const &value,
          VersionNumber const &versionNumber) {
        this->readValue = value;
        this->readVersionNumber = versionNumber;
        if (!immediate) {
          ::callbackRequestProcessCallback(
            &this->processCallback, priorityMedium, this->record);
        }
      },
      [this](bool immediate, std::exception_ptr const& error){
        this->readException = error;
        if (!immediate) {
          ::callbackRequestProcessCallback(
            &this->processCallback, priorityMedium, this->record);
        }
      });
    this->record->pact = true;
    if (immediate) {
      this->template processInternal<T>();
    }
  }

};

// Template specialization for output records.
template<typename RecordType>
class ArrayRecordDeviceSupport<RecordType, RecordDirection::OUTPUT>
    : public detail::ArrayRecordDeviceSupportTrait<RecordType> {

public:

  /**
   * Constructor. Takes a pointer to the record's data-structure.
   */
  ArrayRecordDeviceSupport(RecordType *record)
      : detail::ArrayRecordDeviceSupportTrait<RecordType>(record, record->out) {
    this->template callForValueType<CallInitializeValue>(this);
  }

  /**
   * Starts or completes a write operation (depending on the current state).
   */
  void process() {
    this->template callForValueType<CallProcessInternal>(this);
  }

private:

  /**
   * Helper template for calling the right instantiation of initializedValue for
   * the current value type.
   */
  template<typename T>
  struct CallInitializeValue {
    void operator()(ArrayRecordDeviceSupport *obj) {
      obj->template initializeValue<T>();
    }
  };

  /**
   * Helper template for calling the right instantiation of processInternal for
   * the current value type.
   */
  template<typename T>
  struct CallProcessInternal {
    void operator()(ArrayRecordDeviceSupport *obj) {
      obj->template processInternal<T>();
    }
  };

  /**
   * Exception that occurred while trying to write a value.
   */
  std::exception_ptr writeException;

  /**
   * Tries to initialize the record's value with the initial value of the
   * underlying process variable.
   */
  template<typename T>
  void initializeValue() {
    try {
      auto valueTimeStampAndVersion =
        this->template getPVSupport<T>()->initialValue();
      auto &value = std::get<0>(valueTimeStampAndVersion);
      // We already checked the number of elements in the parent constructor, so
      // if the vector does not have the expected size, something funny is
      // happening.
      if (this->record->nelm != value.size()) {
        std::ostringstream oss;
        oss << "Unexpected got a vector of length " << value.size()
            << " where a vector of length " << this->record->nelm
            << " was expected.";
        throw std::runtime_error(oss.str());
      }
      // When we initialize the value, we also have to allocate the memory. The
      // record support routine only allocates the memory after initializing the
      // device support, but it will gladly use the memory allocated by us.
      if (!this->record->bptr) {
        detail::ArrayRecordBufferHelper<RecordType, T>::initializeBuffer(
          this->record);
      }
      detail::ArrayRecordBufferHelper<RecordType, T>::writeValue(
          this->record, value);
      this->record->nord = this->record->nelm;
      // Reset the UDF flag because we now have a valid value.
      this->record->udf = 0;
    } catch (...) {
      // It might not always be possible to get an initial value, so it is not
      // an error if this fails.
    }
  }

  /**
   * Internal implementation of process(...). This is a template that is
   * instantiated for each supported element type. The calling method ensures
   * that the version for the current element type is called.
   */
  template<typename T>
  void processInternal() {
    // If the record's PACT field is set, this method is called because an
    // asynchronous read completed.
    if (this->record->pact) {
      this->record->pact = false;
      if (this->writeException) {
        auto tempException = this->writeException;
        this->writeException = std::exception_ptr();
        std::rethrow_exception(tempException);
      }
      this->record->nord = this->record->nelm;
      return;
    }

    // Otherwise, this method is called because a value should be written.
    std::vector<T> value(
        detail::ArrayRecordBufferHelper<RecordType, T>::readValue(
            this->record));
    // We can safely pass this to the callback because a record device support
    // is never destroyed once successfully constructed.
    auto pvSupport = this->template getPVSupport<T>();
    bool immediate = pvSupport->write(
      std::move(value),
      [this](bool immediate) {
        if (!immediate) {
          ::callbackRequestProcessCallback(
            &this->processCallback, priorityMedium, this->record);
        }
      },
      [this](bool immediate, std::exception_ptr const& error){
        this->writeException = error;
        if (!immediate) {
          ::callbackRequestProcessCallback(
            &this->processCallback, priorityMedium, this->record);
        }
      });
    this->record->pact = true;
    if (immediate) {
      this->template processInternal<T>();
    }
  }

};

} // namespace EPICS
} // namespace ChimeraTK

#endif // CHIMERATK_EPICS_ARRAY_RECORD_DEVICE_SUPPORT_H
