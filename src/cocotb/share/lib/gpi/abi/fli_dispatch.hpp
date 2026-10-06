// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Dispatch table for the FLI interface when it is compiled into a
// library as a *secondary* interface (COCOTB_FLI_DYN).
//
// See vpi_dispatch.hpp for the general mechanism. All raw headers the
// FLI translation units use are included here before the redirect
// macros so that later includes of them are no-ops and cannot be
// broken by the macros. When COCOTB_FLI_DYN is not defined this header
// does nothing and the entry points are used directly, exactly as
// before.

#ifndef COCOTB_ABI_FLI_DISPATCH_HPP_
#define COCOTB_ABI_FLI_DISPATCH_HPP_

#include <stdint.h>

// Before the vendor headers: mti.h needs the fixed-width integer types but
// only declares them itself on MSVC, includes <inttypes.h> on Linux and
// falls back to <sys/types.h> everywhere else -- and the macOS SDK's
// <sys/types.h> has no uint32_t. The other FLI translation units get it from
// the C++ headers they include first (gpi.h, <map>); this one includes the
// vendor headers before any of them.
#include "_vendor/fli/acc_user.h"
#include "_vendor/fli/acc_vhdl.h"
#include "_vendor/fli/mti.h"
#include "_vendor/tcl/tcl.h"

#ifdef COCOTB_FLI_DYN

namespace cocotb_abi {

struct FliDispatch {
#define COCOTB_ABI_FN(n) decltype(::n) *n;
#include "./fli_fns.inc"
#undef COCOTB_ABI_FN
};

extern FliDispatch fli_disp;

// Resolve every entry point from the simulator's exported symbols.
// Reports and returns false if any symbol is missing.
bool fli_dispatch_fill(void);

}  // namespace cocotb_abi

#ifndef COCOTB_ABI_NO_REDIRECT
#define mti_AddLoadDoneCB (*cocotb_abi::fli_disp.mti_AddLoadDoneCB)
#define mti_AddQuitCB (*cocotb_abi::fli_disp.mti_AddQuitCB)
#define mti_Break (*cocotb_abi::fli_disp.mti_Break)
#define mti_Cmd (*cocotb_abi::fli_disp.mti_Cmd)
#define mti_CreateProcessWithPriority \
    (*cocotb_abi::fli_disp.mti_CreateProcessWithPriority)
#define mti_Delta (*cocotb_abi::fli_disp.mti_Delta)
#define mti_Desensitize (*cocotb_abi::fli_disp.mti_Desensitize)
#define mti_FindRegion (*cocotb_abi::fli_disp.mti_FindRegion)
#define mti_FindSignal (*cocotb_abi::fli_disp.mti_FindSignal)
#define mti_FindVar (*cocotb_abi::fli_disp.mti_FindVar)
#define mti_FirstLowerRegion (*cocotb_abi::fli_disp.mti_FirstLowerRegion)
#define mti_FirstSignal (*cocotb_abi::fli_disp.mti_FirstSignal)
#define mti_FirstVarByRegion (*cocotb_abi::fli_disp.mti_FirstVarByRegion)
#define mti_ForceSignal (*cocotb_abi::fli_disp.mti_ForceSignal)
#define mti_GetArrayElementType \
    (*cocotb_abi::fli_disp.mti_GetArrayElementType)
#define mti_GetArraySignalValue \
    (*cocotb_abi::fli_disp.mti_GetArraySignalValue)
#define mti_GetArrayVarValue (*cocotb_abi::fli_disp.mti_GetArrayVarValue)
#define mti_GetEnumValues (*cocotb_abi::fli_disp.mti_GetEnumValues)
#define mti_GetNumRecordElements \
    (*cocotb_abi::fli_disp.mti_GetNumRecordElements)
#define mti_GetPrimaryName (*cocotb_abi::fli_disp.mti_GetPrimaryName)
#define mti_GetProductVersion (*cocotb_abi::fli_disp.mti_GetProductVersion)
#define mti_GetRegionFullName (*cocotb_abi::fli_disp.mti_GetRegionFullName)
#define mti_GetRegionName (*cocotb_abi::fli_disp.mti_GetRegionName)
#define mti_GetRegionSourceName \
    (*cocotb_abi::fli_disp.mti_GetRegionSourceName)
#define mti_GetResolutionLimit \
    (*cocotb_abi::fli_disp.mti_GetResolutionLimit)
#define mti_GetSignalName (*cocotb_abi::fli_disp.mti_GetSignalName)
#define mti_GetSignalNameIndirect \
    (*cocotb_abi::fli_disp.mti_GetSignalNameIndirect)
#define mti_GetSignalSubelements \
    (*cocotb_abi::fli_disp.mti_GetSignalSubelements)
#define mti_GetSignalType (*cocotb_abi::fli_disp.mti_GetSignalType)
#define mti_GetSignalValue (*cocotb_abi::fli_disp.mti_GetSignalValue)
#define mti_GetSignalValueIndirect \
    (*cocotb_abi::fli_disp.mti_GetSignalValueIndirect)
#define mti_GetTopRegion (*cocotb_abi::fli_disp.mti_GetTopRegion)
#define mti_GetTypeKind (*cocotb_abi::fli_disp.mti_GetTypeKind)
#define mti_GetVarKind (*cocotb_abi::fli_disp.mti_GetVarKind)
#define mti_GetVarName (*cocotb_abi::fli_disp.mti_GetVarName)
#define mti_GetVarSubelements \
    (*cocotb_abi::fli_disp.mti_GetVarSubelements)
#define mti_GetVarType (*cocotb_abi::fli_disp.mti_GetVarType)
#define mti_GetVarValue (*cocotb_abi::fli_disp.mti_GetVarValue)
#define mti_GetVarValueIndirect \
    (*cocotb_abi::fli_disp.mti_GetVarValueIndirect)
#define mti_Interp (*cocotb_abi::fli_disp.mti_Interp)
#define mti_NextRegion (*cocotb_abi::fli_disp.mti_NextRegion)
#define mti_NextSignal (*cocotb_abi::fli_disp.mti_NextSignal)
#define mti_NextVar (*cocotb_abi::fli_disp.mti_NextVar)
#define mti_Now (*cocotb_abi::fli_disp.mti_Now)
#define mti_NowUpper (*cocotb_abi::fli_disp.mti_NowUpper)
#define mti_Quit (*cocotb_abi::fli_disp.mti_Quit)
#define mti_ReleaseSignal (*cocotb_abi::fli_disp.mti_ReleaseSignal)
#define mti_RemoveLoadDoneCB (*cocotb_abi::fli_disp.mti_RemoveLoadDoneCB)
#define mti_RemoveQuitCB (*cocotb_abi::fli_disp.mti_RemoveQuitCB)
#define mti_ScheduleWakeup (*cocotb_abi::fli_disp.mti_ScheduleWakeup)
#define mti_ScheduleWakeup64 (*cocotb_abi::fli_disp.mti_ScheduleWakeup64)
#define mti_Sensitize (*cocotb_abi::fli_disp.mti_Sensitize)
#define mti_SetSignalValue (*cocotb_abi::fli_disp.mti_SetSignalValue)
#define mti_SetVarValue (*cocotb_abi::fli_disp.mti_SetVarValue)
#define mti_TickDir (*cocotb_abi::fli_disp.mti_TickDir)
#define mti_TickLeft (*cocotb_abi::fli_disp.mti_TickLeft)
#define mti_TickLength (*cocotb_abi::fli_disp.mti_TickLength)
#define mti_TickRight (*cocotb_abi::fli_disp.mti_TickRight)
#define mti_VsimFree (*cocotb_abi::fli_disp.mti_VsimFree)
#define acc_fetch_fullname (*cocotb_abi::fli_disp.acc_fetch_fullname)
#define acc_fetch_fulltype (*cocotb_abi::fli_disp.acc_fetch_fulltype)
#define acc_fetch_name (*cocotb_abi::fli_disp.acc_fetch_name)
#define acc_fetch_type (*cocotb_abi::fli_disp.acc_fetch_type)
#define acc_fetch_type_str (*cocotb_abi::fli_disp.acc_fetch_type_str)
#define TclFreeObj (*cocotb_abi::fli_disp.TclFreeObj)

#define Tcl_GetObjResult (*cocotb_abi::fli_disp.Tcl_GetObjResult)
#define Tcl_GetString (*cocotb_abi::fli_disp.Tcl_GetString)
#define Tcl_GetStringResult (*cocotb_abi::fli_disp.Tcl_GetStringResult)
#define Tcl_ListObjGetElements \
    (*cocotb_abi::fli_disp.Tcl_ListObjGetElements)
#define Tcl_ResetResult (*cocotb_abi::fli_disp.Tcl_ResetResult)
#endif  // COCOTB_ABI_NO_REDIRECT

#endif  // COCOTB_FLI_DYN

#endif  // COCOTB_ABI_FLI_DISPATCH_HPP_
