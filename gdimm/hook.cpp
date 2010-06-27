#include "stdafx.h"
#include "hook.h"
#include "dw_text.h"
#include "ft_text.h"
#include "ggo_text.h"
#include "wic_text.h"
#include "gdimm.h"
#include "lock.h"
#include "text_helper.h"

set<HDC> hdc_with_path;
set<gdimm_text **> all_text_instances;
DWORD gdimm_hook::text_tls_index = 0;

__gdi_entry BOOL WINAPI ExtTextOutW_hook( __in HDC hdc, __in int x, __in int y, __in UINT options, __in_opt CONST RECT * lprect, __in_ecount_opt(c) LPCWSTR lpString, __in UINT c, __in_ecount_opt(c) CONST INT * lpDx)
{
	bool b_ret;

//	if (options & ETO_GLYPH_INDEX)
//	if ((options & ETO_GLYPH_INDEX) == 0)
//		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
//	if (c != 3)
//		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	// no text to output
	if (lpString == NULL || c == 0)
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	// rectangle is required but not specified
	// invalid call
	if (((options & ETO_OPAQUE) || (options & ETO_CLIPPED)) && (lprect == NULL))
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
	
	// completely clipped
	if ((options & ETO_CLIPPED) && IsRectEmpty(lprect))
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	// probably a printer
	if (GetDeviceCaps(hdc, TECHNOLOGY) != DT_RASDISPLAY)
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	/*
	the DC use another map mode, which transform the GDI coordination space
	we tried to implement MM_ANISOTROPIC, and found that the text looks worse than the native API
	*/
	if (GetMapMode(hdc) != MM_TEXT)
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	/*
	if ExtTextOut is called within an open path bracket, different draw function is required
	because GDI renders the path outline pretty good, and path is rarely used (one example is Google Earth)
	gdipp does not render HDC with path
	*/
	if (hdc_with_path.find(hdc) != hdc_with_path.end())
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
	
#ifdef _DEBUG
	const wchar_t *debug_text = NULL;
	//debug_text = L"";
	const int start_index = 0;

	if (debug_text != NULL)
	{
		bool is_target = false;
		const int debug_len = (int) wcslen(debug_text);

		if (options & ETO_GLYPH_INDEX)
		{
			WORD *gi = new WORD[debug_len];
			GetGlyphIndicesW(hdc, debug_text, debug_len, gi, 0);

			if (memcmp((WORD *)lpString + start_index, gi, sizeof(WORD) * debug_len) == 0)
				is_target = true;

			delete[] gi;
		}
		else
			is_target = (wcsncmp(lpString + start_index, debug_text, debug_len) == 0);

		if (is_target)
			bool break_now = true;
		else
			return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
	}
#endif // _DEBUG

	// uncomment this to make rendering single-threaded
	//gdimm_lock lock(LOCK_DEBUG);

	gdimm_text::gdimm_text_context context;
	if (!context.init(hdc))
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	if (context.setting_cache->renderer == RENDERER_CLEARTYPE)
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	// gdimm may be attached to a process which already has multiple threads
	// always check if the current thread has text instances
	gdimm_text **text_instances = gdimm_hook::create_tls_text();
	gdimm_text *&curr_text =  text_instances[context.setting_cache->renderer];

	if (curr_text == NULL)
	{
		switch (context.setting_cache->renderer)
		{
		case RENDERER_GETGLYPHOUTLINE:
			curr_text = new gdimm_ggo_text;
			break;
		case RENDERER_DIRECTWRITE:
			curr_text = new gdimm_dw_text;
			break;
		case RENDERER_WIC:
			curr_text = new gdimm_wic_text;
			break;
		default:
			curr_text = new gdimm_ft_text;
			break;
		}
	}

	b_ret = curr_text->begin(&context);

	if (b_ret)
	{
		b_ret = curr_text->text_out(x, y, options, lprect, lpString, c, lpDx);
		curr_text->end();
	}

	if (!b_ret)
		return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

	return TRUE;
}

BOOL WINAPI AbortPath_hook(__in HDC hdc)
{
	BOOL b_ret = AbortPath(hdc);
	if (b_ret)
		hdc_with_path.erase(hdc);

	return b_ret;
}

BOOL WINAPI BeginPath_hook(__in HDC hdc)
{
	BOOL b_ret = BeginPath(hdc);
	if (b_ret)
		hdc_with_path.insert(hdc);

	return b_ret;
}

BOOL WINAPI EndPath_hook(__in HDC hdc)
{
	BOOL b_ret = EndPath(hdc);
	if (b_ret)
		hdc_with_path.erase(hdc);

	return b_ret;
}

#if defined GDIPP_INJECT_SANDBOX && !defined _M_X64
void inject_at_eip(LPPROCESS_INFORMATION lpProcessInformation)
{
	BOOL b_ret;
	DWORD dw_ret;

	// alloc buffer for the injection data
	// the minimum allocation unit is page
	SYSTEM_INFO sys_info;
	GetSystemInfo(&sys_info);
	BYTE *inject_buffer = new BYTE[sys_info.dwPageSize];
	memset(inject_buffer, 0xcc, sys_info.dwPageSize);

	// put gdimm path at the end of the buffer, leave space at the beginning for code
	const DWORD path_offset = sys_info.dwPageSize - MAX_PATH * sizeof(wchar_t);
	dw_ret = GetModuleFileNameW(h_self, (wchar_t *)(inject_buffer + path_offset), MAX_PATH);
	assert(dw_ret != 0);

	// get eip of the spawned thread
	CONTEXT ctx = {};
	ctx.ContextFlags = CONTEXT_CONTROL;
	b_ret = GetThreadContext(lpProcessInformation->hThread, &ctx);
	assert(b_ret);

	LPVOID inject_base = VirtualAllocEx(lpProcessInformation->hProcess, NULL, sys_info.dwPageSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	assert(inject_base != NULL);

	register BYTE *p = inject_buffer;

#define emit_(t, x)	*(t* UNALIGNED) p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)

	emit_db(0x50);		// push eax

	emit_db(0x68);		// push gdimm_path
	emit_dd((DWORD) inject_base + path_offset);
	emit_db(0xB8);		// mov eax, LoadLibraryW
	emit_dd(LoadLibraryW);
	emit_dw(0xD0FF);	// call eax

	emit_db(0x58);		// pop eax -> LoadLibraryW has return value

	emit_db(0x68);		// push original_eip
	emit_dd(ctx.Eip);
	emit_db(0xC3);		// retn -> serve as an absolute jmp

	// write injection data to target process space
	b_ret = WriteProcessMemory(lpProcessInformation->hProcess, inject_base, inject_buffer, sys_info.dwPageSize, NULL);
	assert(b_ret);

	delete[] inject_buffer;

	// notify code change
	b_ret = FlushInstructionCache(lpProcessInformation->hProcess, inject_base, sys_info.dwPageSize);
	assert(b_ret);

	// set eip to the entry point of the injection code
	ctx.Eip = (DWORD) inject_base;
	b_ret = SetThreadContext(lpProcessInformation->hThread, &ctx);
	assert(b_ret);
}

BOOL
WINAPI
CreateProcessAsUserW_hook(
	__in_opt    HANDLE hToken,
	__in_opt    LPCWSTR lpApplicationName,
	__inout_opt LPWSTR lpCommandLine,
	__in_opt    LPSECURITY_ATTRIBUTES lpProcessAttributes,
	__in_opt    LPSECURITY_ATTRIBUTES lpThreadAttributes,
	__in        BOOL bInheritHandles,
	__in        DWORD dwCreationFlags,
	__in_opt    LPVOID lpEnvironment,
	__in_opt    LPCWSTR lpCurrentDirectory,
	__in        LPSTARTUPINFOW lpStartupInfo,
	__out       LPPROCESS_INFORMATION lpProcessInformation)
{
	// if the token is not restricted, redirect the call to original API
	// service can inject
	if (!IsTokenRestricted(hToken))
	{
		return CreateProcessAsUserW(
			hToken,
			lpApplicationName,
			lpCommandLine,
			lpProcessAttributes,
			lpThreadAttributes,
			bInheritHandles,
			dwCreationFlags,
			lpEnvironment,
			lpCurrentDirectory,
			lpStartupInfo,
			lpProcessInformation);
	}

	// otherwise, the spawned process is restricted, and service cannot inject

	// injection at EIP requires the process be suspended
	// if CREATE_SUSPENDED is not specified in the creation flag, remember to resume process after injection
	bool is_suspended;
	if (dwCreationFlags & CREATE_SUSPENDED)
		is_suspended = true;
	else
	{
		is_suspended = false;
		dwCreationFlags |= CREATE_SUSPENDED;
	}

	if (!CreateProcessAsUserW(
		hToken,
		lpApplicationName,
		lpCommandLine,
		lpProcessAttributes,
		lpThreadAttributes,
		bInheritHandles,
		dwCreationFlags,
		lpEnvironment,
		lpCurrentDirectory,
		lpStartupInfo,
		lpProcessInformation))
		return FALSE;

	// since the spawned process can be restricted, EasyHook may not work
	// we inject LoadLibrary call at the entry point of the spawned thread
	inject_at_eip(lpProcessInformation);

	if (!is_suspended)
	{
		DWORD dw_ret = ResumeThread(lpProcessInformation->hThread);
		assert(dw_ret != -1);
	}

	return TRUE;
}
#endif // GDIPP_INJECT_SANDBOX && !_M_X64

// empty exported function to help loading gdimm into target process
__declspec(dllexport) void gdimm_empty_proc()
{
}

// exported function for SetWindowsHookEx
EXTERN_C __declspec(dllexport) LRESULT CALLBACK gdimm_hook_proc(__in int nCode, __in WPARAM wParam, __in LPARAM lParam)
{
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// exported function for EasyHook remote hooking
EXTERN_C __declspec(dllexport) void __stdcall NativeInjectionEntryPoint(REMOTE_ENTRY_INFO* remote_info)
{
	// the process is created suspended, wake it up
	RhWakeUpProcess();
}

gdimm_hook::gdimm_hook()
{
	if (text_tls_index == 0)
		text_tls_index = create_tls_index();
}

gdimm_hook::~gdimm_hook()
{
	free_tls_index(text_tls_index);
}

gdimm_text **gdimm_hook::create_tls_text()
{
	BOOL b_ret;

	gdimm_text **text_instances = (gdimm_text **)TlsGetValue(text_tls_index);
	if (text_instances == NULL)
	{
		text_instances = (gdimm_text **)calloc(_RENDERER_TYPE_COUNT_, sizeof(gdimm_text *));

		b_ret = TlsSetValue(text_tls_index, text_instances);
		assert(b_ret);

		all_text_instances.insert(text_instances);
	}

	return text_instances;
}

void gdimm_hook::delete_tls_text()
{
	gdimm_text **text_instances = (gdimm_text **)TlsGetValue(text_tls_index);
	if (text_instances != NULL)
	{
		all_text_instances.erase(text_instances);

		for (size_t i = 0; i < _RENDERER_TYPE_COUNT_; i++)
		{
			if (text_instances[i] != NULL)
				delete text_instances[i];
		}

		free(text_instances);
	}
}

void gdimm_hook::cleanup()
{
	// when DLL injection and ejection happens, there is no DLL_THREAD_DETACH for the threads
	// we need to clean all remaining thread-related resources, including text instances and font holders

	for (set<gdimm_text **>::const_iterator iter = all_text_instances.begin(); iter != all_text_instances.end(); iter++)
	{
		for (size_t i = 0; i < _RENDERER_TYPE_COUNT_; i++)
		{
			if ((*iter)[i] != NULL)
				delete (*iter)[i];
		}

		free(*iter);
	}
}

bool gdimm_hook::install_hook(LPCTSTR lib_name, LPCSTR proc_name, void *hook_proc)
{
	NTSTATUS eh_error;

	// the target library module must have been loaded in this process before hooking
	const HMODULE h_lib = GetModuleHandle(lib_name);
	if (h_lib == NULL)
		return false;

	TRACED_HOOK_HANDLE h_hook = new HOOK_TRACE_INFO();
	eh_error = LhInstallHook(GetProcAddress(h_lib, proc_name), hook_proc, NULL, h_hook);
	assert(eh_error == 0);

	ULONG thread_id_list[1] = {};
	eh_error = LhSetExclusiveACL(thread_id_list, 0, h_hook);
	assert(eh_error == 0);

	_hooks.push_back(h_hook);

	return true;
}

bool gdimm_hook::hook()
{
	install_hook(TEXT("gdi32.dll"), "ExtTextOutW", ExtTextOutW_hook);
	install_hook(TEXT("gdi32.dll"), "AbortPath", AbortPath_hook);
	install_hook(TEXT("gdi32.dll"), "BeginPath", BeginPath_hook);
	install_hook(TEXT("gdi32.dll"), "EndPath", EndPath_hook);

#if defined GDIPP_INJECT_SANDBOX && !defined _M_X64
	// currently not support inject at EIP for 64-bit processes
	install_hook(TEXT("advapi32.dll"), "CreateProcessAsUserW", CreateProcessAsUserW_hook);
#endif // GDIPP_INJECT_SANDBOX && !_M_X64

	return !(_hooks.empty());
}

void gdimm_hook::unhook()
{
	NTSTATUS eh_error;

	eh_error = LhUninstallAllHooks();
	assert(eh_error == 0);

	eh_error = LhWaitForPendingRemovals();
	assert(eh_error == 0);

	for (list<TRACED_HOOK_HANDLE>::const_iterator iter = _hooks.begin(); iter != _hooks.end(); iter++)
		delete *iter;
}