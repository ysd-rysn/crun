/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019, 2020, 2021 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE

#include <config.h>
#include "../custom-handler.h"
#include "../container.h"
#include "../utils.h"
#include "../linux.h"
#include "handler-utils.h"
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h> // malloc

#ifdef HAVE_DLOPEN
#  include <dlfcn.h>
#endif

#ifdef HAVE_WASMEDGE
#  include <wasmedge/wasmedge.h>
#endif

#if HAVE_DLOPEN && HAVE_WASMEDGE
static int
libwasmedge_load (void **cookie, libcrun_error_t *err arg_unused)
{
  void *handle;

  handle = dlopen ("libwasmedge.so.0", RTLD_NOW);
  if (handle == NULL)
    return crun_make_error (err, 0, "could not load `libwasmedge.so.0`: %s", dlerror ());
  *cookie = handle;

  return 0;
}

static int
libwasmedge_unload (void *cookie, libcrun_error_t *err arg_unused)
{
  int r;

  if (cookie)
    {
      r = dlclose (cookie);
      if (UNLIKELY (r < 0))
        return crun_make_error (err, 0, "could not unload handle: %s", dlerror ());
    }
  return 0;
}

static int
libwasmedge_exec (void *cookie, __attribute__ ((unused)) libcrun_container_t *container, const char *pathname, char *const argv[])
{
  WasmEdge_ConfigureContext *(*WasmEdge_ConfigureCreate) (void);
  void (*WasmEdge_ConfigureDelete) (WasmEdge_ConfigureContext * Cxt);
  void (*WasmEdge_ConfigureAddProposal) (WasmEdge_ConfigureContext * Cxt, const enum WasmEdge_Proposal Prop);
  void (*WasmEdge_ConfigureAddHostRegistration) (WasmEdge_ConfigureContext * Cxt, enum WasmEdge_HostRegistration Host);
  WasmEdge_VMContext *(*WasmEdge_VMCreate) (const WasmEdge_ConfigureContext *ConfCxt, WasmEdge_StoreContext *StoreCxt);
  void (*WasmEdge_VMDelete) (WasmEdge_VMContext * Cxt);
  WasmEdge_Result (*WasmEdge_VMRegisterModuleFromFile) (WasmEdge_VMContext * Cxt, WasmEdge_String ModuleName, const char *Path);
  WasmEdge_Result (*WasmEdge_VMRunWasmFromFile) (WasmEdge_VMContext * Cxt, const char *Path, const WasmEdge_String FuncName, const WasmEdge_Value *Params, const uint32_t ParamLen, WasmEdge_Value *Returns, const uint32_t ReturnLen);
  bool (*WasmEdge_ResultOK) (const WasmEdge_Result Res);
  WasmEdge_String (*WasmEdge_StringCreateByCString) (const char *Str);
  uint32_t argn = 0;
  const char *dirs[1] = { "/:/" };
  WasmEdge_ConfigureContext *configure;
  WasmEdge_VMContext *vm;
  WasmEdge_Result result;

  WasmEdge_ModuleInstanceContext *wasi_module;
  WasmEdge_ModuleInstanceContext *(*WasmEdge_VMGetImportModuleContext) (WasmEdge_VMContext * Cxt, const enum WasmEdge_HostRegistration Reg);
  void (*WasmEdge_ModuleInstanceInitWASI) (WasmEdge_ModuleInstanceContext * Cxt, const char *const *Args, const uint32_t ArgLen, const char *const *Envs, const uint32_t EnvLen, const char *const *Dirs, const uint32_t DirLen, const char *const *Preopens, const uint32_t PreopenLen);
  WasmEdge_ModuleInstanceInitWASI = dlsym (cookie, "WasmEdge_ModuleInstanceInitWASI");

  WasmEdge_ConfigureCreate = dlsym (cookie, "WasmEdge_ConfigureCreate");
  WasmEdge_ConfigureDelete = dlsym (cookie, "WasmEdge_ConfigureDelete");
  WasmEdge_ConfigureAddProposal = dlsym (cookie, "WasmEdge_ConfigureAddProposal");
  WasmEdge_ConfigureAddHostRegistration = dlsym (cookie, "WasmEdge_ConfigureAddHostRegistration");
  WasmEdge_VMCreate = dlsym (cookie, "WasmEdge_VMCreate");
  WasmEdge_VMDelete = dlsym (cookie, "WasmEdge_VMDelete");
  WasmEdge_VMRegisterModuleFromFile = dlsym (cookie, "WasmEdge_VMRegisterModuleFromFile");
  WasmEdge_VMGetImportModuleContext = dlsym (cookie, "WasmEdge_VMGetImportModuleContext");
  WasmEdge_VMRunWasmFromFile = dlsym (cookie, "WasmEdge_VMRunWasmFromFile");
  WasmEdge_ResultOK = dlsym (cookie, "WasmEdge_ResultOK");
  WasmEdge_StringCreateByCString = dlsym (cookie, "WasmEdge_StringCreateByCString");

  if (WasmEdge_ConfigureCreate == NULL || WasmEdge_ConfigureDelete == NULL || WasmEdge_ConfigureAddProposal == NULL
      || WasmEdge_ConfigureAddHostRegistration == NULL || WasmEdge_VMCreate == NULL || WasmEdge_VMDelete == NULL
      || WasmEdge_VMRegisterModuleFromFile == NULL || WasmEdge_VMGetImportModuleContext == NULL
      || WasmEdge_ModuleInstanceInitWASI == NULL || WasmEdge_VMRunWasmFromFile == NULL
      || WasmEdge_ResultOK == NULL || WasmEdge_StringCreateByCString == NULL)
    error (EXIT_FAILURE, 0, "could not find symbol in `libwasmedge.so.0`");

  configure = WasmEdge_ConfigureCreate ();
  if (UNLIKELY (configure == NULL))
    error (EXIT_FAILURE, 0, "could not create wasmedge configure");

  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_BulkMemoryOperations);
  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_ReferenceTypes);
  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_SIMD);
  WasmEdge_ConfigureAddHostRegistration (configure, WasmEdge_HostRegistration_Wasi);

  vm = WasmEdge_VMCreate (configure, NULL);
  if (UNLIKELY (vm == NULL))
    {
      WasmEdge_ConfigureDelete (configure);
      error (EXIT_FAILURE, 0, "could not create wasmedge vm");
    }

  wasi_module = WasmEdge_VMGetImportModuleContext (vm, WasmEdge_HostRegistration_Wasi);
  if (UNLIKELY (wasi_module == NULL))
    {
      WasmEdge_VMDelete (vm);
      WasmEdge_ConfigureDelete (configure);
      error (EXIT_FAILURE, 0, "could not get wasmedge wasi module context");
    }

  for (char *const *arg = argv; *arg != NULL; ++arg, ++argn)
    ;

  WasmEdge_ModuleInstanceInitWASI (wasi_module, (const char *const *) &argv[0], argn, NULL, 0, dirs, 1, NULL, 0);

  result = WasmEdge_VMRunWasmFromFile (vm, pathname, WasmEdge_StringCreateByCString ("_start"), NULL, 0, NULL, 0);

  if (UNLIKELY (! WasmEdge_ResultOK (result)))
    {
      WasmEdge_VMDelete (vm);
      WasmEdge_ConfigureDelete (configure);
      error (EXIT_FAILURE, 0, "could not get wasmedge result from VM");
    }

  WasmEdge_VMDelete (vm);
  WasmEdge_ConfigureDelete (configure);
  exit (EXIT_SUCCESS);
}

static int
libwasmedge_exec_multiple_wasm (void *cookie, __attribute__ ((unused)) libcrun_container_t *container, const char *pathname, char *const argv[])
{
  WasmEdge_ConfigureContext *(*WasmEdge_ConfigureCreate) (void);
  void (*WasmEdge_ConfigureDelete) (WasmEdge_ConfigureContext * Cxt);
  void (*WasmEdge_ConfigureAddProposal) (WasmEdge_ConfigureContext * Cxt, const enum WasmEdge_Proposal Prop);
  void (*WasmEdge_ConfigureAddHostRegistration) (WasmEdge_ConfigureContext * Cxt, enum WasmEdge_HostRegistration Host);
  WasmEdge_VMContext *(*WasmEdge_VMCreate) (const WasmEdge_ConfigureContext *ConfCxt, WasmEdge_StoreContext *StoreCxt);
  void (*WasmEdge_VMDelete) (WasmEdge_VMContext * Cxt);
  void (*WasmEdge_AsyncWait) (const WasmEdge_Async *Cxt);
  WasmEdge_Result (*WasmEdge_AsyncGet) (const WasmEdge_Async *Cxt, WasmEdge_Value *Returns, const uint32_t ReturnLen);
  void (*WasmEdge_AsyncDelete) (WasmEdge_Async *Cxt);
  WasmEdge_Async *(*WasmEdge_VMAsyncRunWasmFromFile) (WasmEdge_VMContext *Cxt, const char *Path, const WasmEdge_String FuncName, const WasmEdge_Value *Params, const uint32_t ParamLen);
  uint32_t (*WasmEdge_AsyncGetReturnsLength) (const WasmEdge_Async *Cxt);
  WasmEdge_Result (*WasmEdge_VMRegisterModuleFromFile) (WasmEdge_VMContext * Cxt, WasmEdge_String ModuleName, const char *Path);
  WasmEdge_Result (*WasmEdge_VMRunWasmFromFile) (WasmEdge_VMContext * Cxt, const char *Path, const WasmEdge_String FuncName, const WasmEdge_Value *Params, const uint32_t ParamLen, WasmEdge_Value *Returns, const uint32_t ReturnLen);
  bool (*WasmEdge_ResultOK) (const WasmEdge_Result Res);
  WasmEdge_String (*WasmEdge_StringCreateByCString) (const char *Str);
  WasmEdge_ModuleInstanceContext *(*WasmEdge_VMGetImportModuleContext) (WasmEdge_VMContext * Cxt, const enum WasmEdge_HostRegistration Reg);
  void (*WasmEdge_ModuleInstanceInitWASI) (WasmEdge_ModuleInstanceContext * Cxt, const char *const *Args, const uint32_t ArgLen, const char *const *Envs, const uint32_t EnvLen, const char *const *Preopens, const uint32_t PreopenLen);

  WasmEdge_ModuleInstanceInitWASI = dlsym (cookie, "WasmEdge_ModuleInstanceInitWASI");
  WasmEdge_ConfigureCreate = dlsym (cookie, "WasmEdge_ConfigureCreate");
  WasmEdge_ConfigureDelete = dlsym (cookie, "WasmEdge_ConfigureDelete");
  WasmEdge_ConfigureAddProposal = dlsym (cookie, "WasmEdge_ConfigureAddProposal");
  WasmEdge_ConfigureAddHostRegistration = dlsym (cookie, "WasmEdge_ConfigureAddHostRegistration");
  WasmEdge_VMCreate = dlsym (cookie, "WasmEdge_VMCreate");
  WasmEdge_VMDelete = dlsym (cookie, "WasmEdge_VMDelete");
  WasmEdge_AsyncWait = dlsym (cookie, "WasmEdge_AsyncWait");
  WasmEdge_AsyncGet = dlsym (cookie, "WasmEdge_AsyncGet");
  WasmEdge_AsyncDelete = dlsym (cookie, "WasmEdge_AsyncDelete");
  WasmEdge_AsyncGetReturnsLength = dlsym (cookie, "WasmEdge_AsyncGetReturnsLength");
  WasmEdge_VMAsyncRunWasmFromFile = dlsym (cookie, "WasmEdge_VMAsyncRunWasmFromFile");
  WasmEdge_VMRegisterModuleFromFile = dlsym (cookie, "WasmEdge_VMRegisterModuleFromFile");
  WasmEdge_VMGetImportModuleContext = dlsym (cookie, "WasmEdge_VMGetImportModuleContext");
  WasmEdge_VMRunWasmFromFile = dlsym (cookie, "WasmEdge_VMRunWasmFromFile");
  WasmEdge_ResultOK = dlsym (cookie, "WasmEdge_ResultOK");
  WasmEdge_StringCreateByCString = dlsym (cookie, "WasmEdge_StringCreateByCString");

  if (WasmEdge_ConfigureCreate == NULL || WasmEdge_ConfigureDelete == NULL || WasmEdge_ConfigureAddProposal == NULL
      || WasmEdge_ConfigureAddHostRegistration == NULL || WasmEdge_VMCreate == NULL || WasmEdge_VMDelete == NULL
	  || WasmEdge_AsyncWait == NULL || WasmEdge_AsyncGet == NULL || WasmEdge_AsyncDelete == NULL || WasmEdge_VMAsyncRunWasmFromFile == NULL
      || WasmEdge_AsyncGetReturnsLength == NULL || WasmEdge_VMRegisterModuleFromFile == NULL || WasmEdge_VMGetImportModuleContext == NULL
      || WasmEdge_ModuleInstanceInitWASI == NULL || WasmEdge_VMRunWasmFromFile == NULL
      || WasmEdge_ResultOK == NULL || WasmEdge_StringCreateByCString == NULL)
    error (EXIT_FAILURE, 0, "could not find symbol in `libwasmedge.so.0`");

  uint32_t n_wasm = 0; // Number of wasm files.
  // Count wasm files.
  for (char *const *arg = argv; *arg != NULL; ++arg, ++n_wasm)
    ;

  // TODO: Create directory map for each wasm.
  const char *dirs[1] = { "/:/" };

  WasmEdge_ConfigureContext *configure;
  WasmEdge_VMContext **vm = (WasmEdge_VMContext **) malloc (n_wasm * sizeof (WasmEdge_VMContext *));
  WasmEdge_Async **async = (WasmEdge_Async **) malloc (n_wasm * sizeof (WasmEdge_Async *));
  WasmEdge_Result *result = (WasmEdge_Result *) malloc (n_wasm * sizeof (WasmEdge_Result));
  WasmEdge_ModuleInstanceContext **wasi_module = (WasmEdge_ModuleInstanceContext **) malloc (n_wasm * sizeof (WasmEdge_ModuleInstanceContext *));
  uint32_t *arity = (uint32_t *) malloc (n_wasm * sizeof (uint32_t));

  // Create config.
  configure = WasmEdge_ConfigureCreate ();
  if (UNLIKELY (configure == NULL))
    {
	  // TODO: Deallocate all resources.
	  error (EXIT_FAILURE, 0, "could not create wasmedge configure");
	}

  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_BulkMemoryOperations);
  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_ReferenceTypes);
  WasmEdge_ConfigureAddProposal (configure, WasmEdge_Proposal_SIMD);
  WasmEdge_ConfigureAddHostRegistration (configure, WasmEdge_HostRegistration_Wasi);

  for (int i = 0; i < n_wasm; i++)
    {
	  // Create vm.
	  vm[i] = WasmEdge_VMCreate (configure, NULL);
	  if (UNLIKELY (vm[i] == NULL))
		{
		  // TODO: Deallocate all resources.
		  error (EXIT_FAILURE, 0, "could not create wasmedge vm");
		}

      wasi_module[i] = WasmEdge_VMGetImportModuleContext (vm[i], WasmEdge_HostRegistration_Wasi);
	  if (UNLIKELY (wasi_module[i] == NULL))
		{
		  // TODO: Deallocate all resources.
		  error (EXIT_FAILURE, 0, "could not get wasmedge wasi module context");
		}

      // Initialize wasi module.
      WasmEdge_ModuleInstanceInitWASI (wasi_module[i], (const char *const *) &argv[i], 1, NULL, 0, dirs, 1);

      // Run wasm asynchronously.
	  async[i] = WasmEdge_VMAsyncRunWasmFromFile (vm[i], argv[i], WasmEdge_StringCreateByCString ("_start"), NULL, 0);
	  if (UNLIKELY (async[i] == NULL))
	    {
		  // TODO: Deallocate all resources.
		  error (EXIT_FAILURE, 0, "could not get wasmedge async object");
	    }
	}

  for (int i = 0; i < n_wasm; i++)
	{
      // Wait for the execution.
	  WasmEdge_AsyncWait (async[i]);

	  // Check the returns length.
      arity[i] = WasmEdge_AsyncGetReturnsLength (async[i]);

	  // Get the result.
      result[i] = WasmEdge_AsyncGet (async[i], NULL, arity[i]);
	  if (UNLIKELY (! WasmEdge_ResultOK(result[i])))
		{
		  // TODO: Deallocate all resources.
		  error (EXIT_FAILURE, 0, "could not get wasmedge result from VM");
		}
	}

  // Deallocate all resources.
  for (int i = 0; i < n_wasm; i++)
	{
	  WasmEdge_AsyncDelete (async[i]);
      WasmEdge_VMDelete (vm[i]);
	}
  WasmEdge_ConfigureDelete (configure);
  free (vm);
  free (async);
  free (result);
  free (wasi_module);
  free (arity);
  exit (EXIT_SUCCESS);
}

static int
wasmedge_can_handle_container (libcrun_container_t *container, libcrun_error_t *err arg_unused)
{
  return wasm_can_handle_container (container, err);
}

static int
wasmedge_can_handle_multiple_wasm (libcrun_container_t *container, libcrun_error_t *err arg_unused)
{
  if (wasm_can_handle_container (container, err))
	{
	  return can_handle_multiple_wasm (container, err);
	}

  return 0;
}

struct custom_handler_s handler_wasmedge = {
  .name = "wasmedge",
  .feature_string = "WASM:wasmedge",
  .load = libwasmedge_load,
  .unload = libwasmedge_unload,
  .exec_func = libwasmedge_exec,
  .can_handle_container = wasmedge_can_handle_container,
};

struct custom_handler_s handler_wasmedge_with_multiple_wasm = {
  .name = "wasmedge_with_multiple_wasm",
  .feature_string = "WASM:wasmedge_with_multiple_wasm",
  .load = libwasmedge_load,
  .unload = libwasmedge_unload,
  .exec_func = libwasmedge_exec_multiple_wasm,
  .can_handle_container = wasmedge_can_handle_container, // TODO: Switch to wasmedge_can_handle_multiple_wasm.
};

#endif
