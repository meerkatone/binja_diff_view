use crate::{BinaryDiffEngine, PreparedBinary};
use log::error;
use std::ffi::c_void;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};

fn guard<T>(default: T, f: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => v,
        Err(_) => {
            error!("Panic caught in FFI boundary");
            default
        }
    }
}

/// Prepare an immutable process-local snapshot; free with `rust_diff_free_prepared_v1`.
/// Returns null on cancellation or invalid JSON. The source string is not retained.
///
/// # Safety
/// `json` must be a valid NUL-terminated UTF-8 string. The optional cancellation
/// callback and context must be valid and thread-safe for this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_prepare_json_v1(
    json: *const c_char,
    cancel: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    context: *mut c_void,
) -> *mut PreparedBinary {
    guard(std::ptr::null_mut(), || {
        if json.is_null() {
            return std::ptr::null_mut();
        }
        let Ok(json) = (unsafe { CStr::from_ptr(json) }).to_str() else {
            return std::ptr::null_mut();
        };
        let address = context as usize;
        let cancelled = || cancel.is_some_and(|f| unsafe { f(address as *mut c_void) });
        match PreparedBinary::from_json(json, &cancelled) {
            Ok(snapshot) => Box::into_raw(Box::new(snapshot)),
            Err(error) => {
                error!("Snapshot preparation failed: {}", error);
                std::ptr::null_mut()
            }
        }
    })
}

/// Release an owned snapshot. Null is accepted.
///
/// # Safety
/// The pointer must be null or a live handle returned by `rust_diff_prepare_json_v1`.
/// Free exactly once, after every concurrent comparison using it has completed.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_free_prepared_v1(snapshot: *mut PreparedBinary) {
    guard((), || {
        if !snapshot.is_null() {
            drop(unsafe { Box::from_raw(snapshot) });
        }
    });
}

/// Return the retained snapshot's estimated allocation capacity in bytes.
///
/// # Safety
/// The handle must be null or live for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_prepared_size_v1(snapshot: *const PreparedBinary) -> usize {
    guard(0, || {
        unsafe { snapshot.as_ref() }.map_or(0, PreparedBinary::estimated_bytes)
    })
}

/// Compare immutable prepared snapshots, returning the existing JSON result schema.
///
/// # Safety
/// Handles must be live for the whole synchronous call. Concurrent reads are
/// allowed; neither handle may be freed until all readers complete. Callback
/// requirements are the same as `rust_diff_prepare_json_v1`.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_diff_prepared_v1(
    a: *const PreparedBinary,
    b: *const PreparedBinary,
    cancel: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    context: *mut c_void,
) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        let (Some(a), Some(b)) = (unsafe { a.as_ref() }, unsafe { b.as_ref() }) else {
            return std::ptr::null_mut();
        };
        let address = context as usize;
        let cancelled = || cancel.is_some_and(|f| unsafe { f(address as *mut c_void) });
        let _ = env_logger::try_init();
        match BinaryDiffEngine::new()
            .perform_diff_prepared(a, b, &cancelled)
            .and_then(|result| Ok(CString::new(serde_json::to_string(&result)?)?))
        {
            Ok(json) => json.into_raw(),
            Err(error) => {
                error!("Prepared diff failed: {}", error);
                std::ptr::null_mut()
            }
        }
    })
}

/// Diff two binaries from JSON-encoded function arrays and return the full
/// DiffResult as a JSON C string (caller frees with rust_diff_free_string).
/// Returns null on parse/diff failure. This is the entry point the Binary
/// Ninja Python frontend uses; no engine handle is needed.
///
/// # Safety
/// Both pointers must reference valid, NUL-terminated UTF-8 strings for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_diff_json(
    functions_a_json: *const c_char,
    functions_b_json: *const c_char,
) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        if functions_a_json.is_null() || functions_b_json.is_null() {
            return std::ptr::null_mut();
        }

        let json_a = match unsafe { CStr::from_ptr(functions_a_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };
        let json_b = match unsafe { CStr::from_ptr(functions_b_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };

        let _ = env_logger::try_init();
        let engine = BinaryDiffEngine::new();
        let result = match engine.perform_diff_json(json_a, json_b) {
            Ok(r) => r,
            Err(e) => {
                error!("JSON diff failed: {}", e);
                return std::ptr::null_mut();
            }
        };

        match serde_json::to_string(&result) {
            Ok(s) => match CString::new(s) {
                Ok(cs) => cs.into_raw(),
                Err(_) => std::ptr::null_mut(),
            },
            Err(e) => {
                error!("Failed to serialize diff result: {}", e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Cancellable whole-binary diff used by the native UI. `cancel` may be null;
/// when present it is polled between matching phases and inside the parallel
/// fuzzy candidate loop.
///
/// # Safety
/// The JSON pointers follow `rust_diff_diff_json`. `cancel_context` must remain
/// valid for every invocation of `cancel` during this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_diff_json_cancellable(
    functions_a_json: *const c_char,
    functions_b_json: *const c_char,
    cancel: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    cancel_context: *mut c_void,
) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        if functions_a_json.is_null() || functions_b_json.is_null() {
            return std::ptr::null_mut();
        }
        let json_a = match unsafe { CStr::from_ptr(functions_a_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };
        let json_b = match unsafe { CStr::from_ptr(functions_b_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };

        // Raw pointers do not implement Sync. Capturing the opaque address as
        // an integer lets Rayon workers poll the caller-owned, thread-safe task.
        let context_address = cancel_context as usize;
        let cancelled = || match cancel {
            Some(callback) => unsafe { callback(context_address as *mut c_void) },
            None => false,
        };
        let _ = env_logger::try_init();
        let engine = BinaryDiffEngine::new();
        let result = match engine.perform_diff_json_cancellable(json_a, json_b, &cancelled) {
            Ok(result) => result,
            Err(error) => {
                error!("Cancellable JSON diff failed: {}", error);
                return std::ptr::null_mut();
            }
        };
        match serde_json::to_string(&result)
            .ok()
            .and_then(|json| CString::new(json).ok())
        {
            Some(json) => json.into_raw(),
            None => std::ptr::null_mut(),
        }
    })
}

/// Semantic IL diff between two functions. `request_json` is an
/// `il::IlDiffRequest` (two IL token streams plus an optional callee rename
/// map); returns an `il::IlDiff` as a JSON C string (caller frees with
/// rust_diff_free_string), or null on parse failure. Unlike a textual diff,
/// this normalizes volatile tokens and resolves matched-callee renames, so a
/// renamed call is reported as cosmetic while a replaced call is a real change.
/// Callable directly from the Python frontend at diff-view time.
///
/// # Safety
/// `request_json` must reference a valid, NUL-terminated UTF-8 string for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_il_diff_json(request_json: *const c_char) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        if request_json.is_null() {
            return std::ptr::null_mut();
        }
        let json = match unsafe { CStr::from_ptr(request_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };
        let request: crate::il::IlDiffRequest = match serde_json::from_str(json) {
            Ok(r) => r,
            Err(e) => {
                error!("Failed to parse IL diff request: {}", e);
                return std::ptr::null_mut();
            }
        };
        let diff = crate::il::diff_il(&request);
        match serde_json::to_string(&diff) {
            Ok(s) => match CString::new(s) {
                Ok(cs) => cs.into_raw(),
                Err(_) => std::ptr::null_mut(),
            },
            Err(e) => {
                error!("Failed to serialize IL diff: {}", e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Basic-block correspondence between two matched functions. `request_json`
/// is a `block::BlockDiffRequest` (two functions as IL basic blocks with CFG
/// successors, plus the optional rename/addr maps); returns a
/// `block::BlockDiff` as a JSON C string (caller frees with
/// rust_diff_free_string), or null on parse failure:
/// `{ pairs: [{a, b, status, similarity}], only_a: [...], only_b: [...], similarity }`.
/// Drives the graph diff overlay: per-pair status is equal / cosmetic / changed.
///
/// # Safety
/// `request_json` must reference a valid, NUL-terminated UTF-8 string for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_block_diff_json(request_json: *const c_char) -> *mut c_char {
    guard(std::ptr::null_mut(), || {
        if request_json.is_null() {
            return std::ptr::null_mut();
        }
        let json = match unsafe { CStr::from_ptr(request_json) }.to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };
        let request: crate::block::BlockDiffRequest = match serde_json::from_str(json) {
            Ok(r) => r,
            Err(e) => {
                error!("Failed to parse block diff request: {}", e);
                return std::ptr::null_mut();
            }
        };
        let diff = crate::block::block_diff(&request);
        match serde_json::to_string(&diff) {
            Ok(s) => match CString::new(s) {
                Ok(cs) => cs.into_raw(),
                Err(_) => std::ptr::null_mut(),
            },
            Err(e) => {
                error!("Failed to serialize block diff: {}", e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Free a string returned by rust_diff_diff_json.
///
/// # Safety
/// `s` must be null or a pointer returned by one of this library's JSON FFI
/// functions, and it must be freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn rust_diff_free_string(s: *mut c_char) {
    guard((), || {
        if !s.is_null() {
            unsafe {
                let _ = CString::from_raw(s);
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn always_cancel(_: *mut c_void) -> bool {
        true
    }

    #[test]
    fn ffi_rejects_null_inputs() {
        let result = unsafe { rust_diff_diff_json(std::ptr::null(), std::ptr::null()) };
        assert!(result.is_null());
        unsafe {
            assert!(
                rust_diff_prepare_json_v1(std::ptr::null(), None, std::ptr::null_mut()).is_null()
            );
            assert_eq!(rust_diff_prepared_size_v1(std::ptr::null()), 0);
            assert!(rust_diff_diff_prepared_v1(
                std::ptr::null(),
                std::ptr::null(),
                None,
                std::ptr::null_mut()
            )
            .is_null());
            rust_diff_free_prepared_v1(std::ptr::null_mut());
        }
    }

    #[test]
    fn ffi_cancellable_diff_stops() {
        let empty = CString::new("[]").unwrap();
        let result = unsafe {
            rust_diff_diff_json_cancellable(
                empty.as_ptr(),
                empty.as_ptr(),
                Some(always_cancel),
                std::ptr::null_mut(),
            )
        };
        assert!(result.is_null());
        assert!(unsafe {
            rust_diff_prepare_json_v1(empty.as_ptr(), Some(always_cancel), std::ptr::null_mut())
        }
        .is_null());
    }

    #[test]
    fn prepared_handle_owns_input_and_survives_cancelled_comparison() {
        unsafe {
            let text = CString::new(
                r#"[{"name":"leaf","address":1,"instructions":[{"mnemonic":"ret"}]}]"#,
            )
            .unwrap();
            let snapshot = rust_diff_prepare_json_v1(text.as_ptr(), None, std::ptr::null_mut());
            assert!(!snapshot.is_null());
            drop(text);
            assert!(rust_diff_prepared_size_v1(snapshot) > std::mem::size_of::<PreparedBinary>());
            assert!(rust_diff_diff_prepared_v1(
                snapshot,
                snapshot,
                Some(always_cancel),
                std::ptr::null_mut()
            )
            .is_null());
            for _ in 0..3 {
                let result =
                    rust_diff_diff_prepared_v1(snapshot, snapshot, None, std::ptr::null_mut());
                assert!(!result.is_null());
                let json: serde_json::Value =
                    serde_json::from_slice(CStr::from_ptr(result).to_bytes()).unwrap();
                assert_eq!(json["matched_functions"].as_array().unwrap().len(), 1);
                rust_diff_free_string(result);
            }
            rust_diff_free_prepared_v1(snapshot);
            let invalid = CString::new("invalid json").unwrap();
            assert!(
                rust_diff_prepare_json_v1(invalid.as_ptr(), None, std::ptr::null_mut()).is_null()
            );
        }
    }
}
