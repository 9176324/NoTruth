// Copyright (c) 2016-2017, KelvinChan. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Declares interfaces to VMM initialization functions

#ifndef HYPERPLATFORM_VM_H_
#define HYPERPLATFORM_VM_H_

#include <fltKernel.h>

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

/// Virtualizes all processors
/// @return STATUS_SUCCESS on success
///
/// Initializes a VMCS region and virtualizes (ie, enters the VMX non-root
/// operation mode) for each processor. Returns non STATUS_SUCCESS value if any
/// of processors failed to do so. In that case, this function de-virtualize
/// already virtualized processors.
_IRQL_requires_max_(PASSIVE_LEVEL) NTSTATUS VmInitialization();

/// De-virtualize all processors
_IRQL_requires_max_(PASSIVE_LEVEL) void VmTermination();

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

}  // extern "C"

#endif  // HYPERPLATFORM_VM_H_
