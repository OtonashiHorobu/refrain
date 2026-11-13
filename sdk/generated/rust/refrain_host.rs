// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9acb7bb41c6b4563

// Rust bindings for the sealed Refrain host contracts.
// C ABI layout is authoritative; this file mirrors it.
#![allow(non_camel_case_types, non_snake_case, dead_code)]

#[repr(C)]
pub struct RefrainStr {
    pub data: *const u8,
    pub len: u64,
}

#[repr(C)]
pub struct RefrainBytes {
    pub data: *const u8,
    pub len: u64,
}

#[repr(C)]
pub struct RefrainMutBytes {
    pub data: *mut u8,
    pub len: u64,
}

#[repr(C)]
pub struct RefrainSlice<T> {
    pub data: *const T,
    pub len: u64,
}

pub type RefrainStatus = u32;
pub const REFRAIN_STATUS_OK: RefrainStatus = 0;
pub const REFRAIN_HANDLE_INVALID: u64 = 0;

pub mod core {
    use super::*;

    pub type host_error = u32;
    pub const HOST_ERROR_INVALID_ARGUMENT: host_error = 1;
    pub const HOST_ERROR_NOT_FOUND: host_error = 2;
    pub const HOST_ERROR_PERMISSION_DENIED: host_error = 3;
    pub const HOST_ERROR_VERSION_MISMATCH: host_error = 4;
    pub const HOST_ERROR_UNSUPPORTED: host_error = 5;
    pub const HOST_ERROR_NOT_INITIALIZED: host_error = 6;
    pub const HOST_ERROR_OUT_OF_MEMORY: host_error = 7;
    pub const HOST_ERROR_LIMIT_EXCEEDED: host_error = 8;
    pub const HOST_ERROR_THREAD_VIOLATION: host_error = 9;
    pub const HOST_ERROR_INTERNAL: host_error = 10;
    pub const HOST_ERROR_ALREADY_EXISTS: host_error = 11;
    pub const HOST_ERROR_IO_ERROR: host_error = 12;

    #[repr(C)]
    pub struct host_version {
        pub struct_size: u32,
        pub major: u32,
        pub minor: u32,
        pub patch: u32,
    }

    #[repr(C)]
    pub struct provider_ref {
        pub struct_size: u32,
        pub vtable: *mut ::core::ffi::c_void,
        pub self_: *mut ::core::ffi::c_void,
        pub abi_major: u32,
        pub reserved: u32,
    }

    const _: () = assert!(::core::mem::size_of::<host_version>() == 16);
    const _: () = assert!(::core::mem::align_of::<host_version>() == 4);
    const _: () = assert!(::core::mem::offset_of!(host_version, major) == 4);
    const _: () = assert!(::core::mem::offset_of!(host_version, minor) == 8);
    const _: () = assert!(::core::mem::offset_of!(host_version, patch) == 12);

    const _: () = assert!(::core::mem::size_of::<provider_ref>() == 32);
    const _: () = assert!(::core::mem::align_of::<provider_ref>() == 8);
    const _: () = assert!(::core::mem::offset_of!(provider_ref, vtable) == 8);
    const _: () = assert!(::core::mem::offset_of!(provider_ref, self_) == 16);
    const _: () = assert!(::core::mem::offset_of!(provider_ref, abi_major) == 24);
    const _: () = assert!(::core::mem::offset_of!(provider_ref, reserved) == 28);

    #[repr(C)]
    pub struct refrain_host_core_vtable {
        pub struct_size: u32,
        pub abi_major: u32,
        pub abi_minor: u32,
        pub abi_patch: u32,
        pub version: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, value: *mut host_version) -> ()>,
        pub log: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, level: u32, message: RefrainStr) -> ()>,
        pub now_ns: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> i64>,
        pub tick_now: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> i64>,
        pub config_get_i64: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, ns: RefrainStr, key: RefrainStr, out_value: *mut i64) -> RefrainStatus>,
        pub config_get_bool: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, ns: RefrainStr, key: RefrainStr, out_value: *mut u8) -> RefrainStatus>,
        pub config_get_string: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, ns: RefrainStr, key: RefrainStr, out_value: *mut RefrainStr) -> RefrainStatus>,
        pub query_service: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, service: RefrainStr, instance: RefrainStr, out_value: *mut provider_ref) -> RefrainStatus>,
        pub register_provider: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, service: RefrainStr, instance: RefrainStr, r#ref: *const provider_ref) -> RefrainStatus>,
        pub alloc: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, size: u64, alignment: u64, out_value: *mut RefrainMutBytes) -> RefrainStatus>,
        pub free: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, block: RefrainMutBytes) -> ()>,
        pub last_error_message: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, out_value: *mut RefrainStr) -> RefrainStatus>,
    }

    pub const REFRAIN_HOST_CORE_SERVICE_ID: &str = "refrain:host.core";
    pub const REFRAIN_HOST_CORE_ABI_MAJOR: u32 = 1;
    pub const REFRAIN_HOST_CORE_ABI_MINOR: u32 = 0;
    pub const REFRAIN_HOST_CORE_ABI_PATCH: u32 = 0;

} // pub mod core

pub mod kernel {
    use super::*;

    pub type kernel_error = u32;
    pub const KERNEL_ERROR_INVALID_ARGUMENT: kernel_error = 1;
    pub const KERNEL_ERROR_NOT_FOUND: kernel_error = 2;
    pub const KERNEL_ERROR_PERMISSION_DENIED: kernel_error = 3;
    pub const KERNEL_ERROR_UNSUPPORTED: kernel_error = 4;
    pub const KERNEL_ERROR_OUT_OF_MEMORY: kernel_error = 5;
    pub const KERNEL_ERROR_CANCELLED: kernel_error = 6;
    pub const KERNEL_ERROR_TIMEOUT: kernel_error = 7;
    pub const KERNEL_ERROR_THREAD_VIOLATION: kernel_error = 8;
    pub const KERNEL_ERROR_INTERNAL: kernel_error = 9;

    pub type job = u64;
    pub const JOB_INVALID: job = 0;

    pub type subscription = u64;
    pub const SUBSCRIPTION_INVALID: subscription = 0;

    pub type job_callback = Option<unsafe extern "C" fn(userdata: *mut ::core::ffi::c_void)>;

    pub type event_callback = Option<unsafe extern "C" fn(userdata: *mut ::core::ffi::c_void, payload: *mut ::core::ffi::c_void, size: u64)>;

    #[repr(C)]
    pub struct refrain_host_kernel_vtable {
        pub struct_size: u32,
        pub abi_major: u32,
        pub abi_minor: u32,
        pub abi_patch: u32,
        pub job_submit: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, priority: u32, callback: job_callback, userdata: *mut ::core::ffi::c_void, out_value: *mut job) -> RefrainStatus>,
        pub job_cancel: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: job) -> RefrainStatus>,
        pub job_wait: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: job) -> RefrainStatus>,
        pub event_subscribe: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, event_type: u64, callback: event_callback, userdata: *mut ::core::ffi::c_void, out_value: *mut subscription) -> RefrainStatus>,
        pub event_unsubscribe: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: subscription) -> RefrainStatus>,
        pub event_publish: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, event_type: u64, payload: *mut ::core::ffi::c_void, size: u64) -> ()>,
    }

    pub const REFRAIN_HOST_KERNEL_SERVICE_ID: &str = "refrain:host.kernel";
    pub const REFRAIN_HOST_KERNEL_ABI_MAJOR: u32 = 1;
    pub const REFRAIN_HOST_KERNEL_ABI_MINOR: u32 = 0;
    pub const REFRAIN_HOST_KERNEL_ABI_PATCH: u32 = 0;

} // pub mod kernel

pub mod metadata {
    use super::*;

    pub type metadata_error = u32;
    pub const METADATA_ERROR_INVALID_ARGUMENT: metadata_error = 1;
    pub const METADATA_ERROR_NOT_FOUND: metadata_error = 2;
    pub const METADATA_ERROR_INTERNAL: metadata_error = 3;

    pub type type_kind = u32;
    pub const TYPE_KIND_STRUCTURE: type_kind = 0;
    pub const TYPE_KIND_ENUMERATION: type_kind = 1;
    pub const TYPE_KIND_FLAGS: type_kind = 2;
    pub const TYPE_KIND_RECORD: type_kind = 3;
    pub const TYPE_KIND_PACKET: type_kind = 4;

    #[repr(C)]
    pub struct type_info {
        pub struct_size: u32,
        pub id: RefrainStr,
        pub kind: type_kind,
        pub size: u32,
        pub alignment: u32,
        pub field_count: u32,
        pub enum_count: u32,
    }

    #[repr(C)]
    pub struct field_info {
        pub struct_size: u32,
        pub name: RefrainStr,
        pub type_id: RefrainStr,
        pub offset: u32,
        pub size: u32,
        pub flags: u32,
    }

    #[repr(C)]
    pub struct enum_value_info {
        pub struct_size: u32,
        pub name: RefrainStr,
        pub value: i64,
    }

    #[repr(C)]
    pub struct contract_info {
        pub struct_size: u32,
        pub id: RefrainStr,
        pub abi_major: u32,
        pub abi_minor: u32,
        pub abi_patch: u32,
        pub schema_hash: RefrainStr,
        pub method_count: u32,
    }

    const _: () = assert!(::core::mem::size_of::<type_info>() == 48);
    const _: () = assert!(::core::mem::align_of::<type_info>() == 8);
    const _: () = assert!(::core::mem::offset_of!(type_info, id) == 8);
    const _: () = assert!(::core::mem::offset_of!(type_info, kind) == 24);
    const _: () = assert!(::core::mem::offset_of!(type_info, size) == 28);
    const _: () = assert!(::core::mem::offset_of!(type_info, alignment) == 32);
    const _: () = assert!(::core::mem::offset_of!(type_info, field_count) == 36);
    const _: () = assert!(::core::mem::offset_of!(type_info, enum_count) == 40);

    const _: () = assert!(::core::mem::size_of::<field_info>() == 56);
    const _: () = assert!(::core::mem::align_of::<field_info>() == 8);
    const _: () = assert!(::core::mem::offset_of!(field_info, name) == 8);
    const _: () = assert!(::core::mem::offset_of!(field_info, type_id) == 24);
    const _: () = assert!(::core::mem::offset_of!(field_info, offset) == 40);
    const _: () = assert!(::core::mem::offset_of!(field_info, size) == 44);
    const _: () = assert!(::core::mem::offset_of!(field_info, flags) == 48);

    const _: () = assert!(::core::mem::size_of::<enum_value_info>() == 32);
    const _: () = assert!(::core::mem::align_of::<enum_value_info>() == 8);
    const _: () = assert!(::core::mem::offset_of!(enum_value_info, name) == 8);
    const _: () = assert!(::core::mem::offset_of!(enum_value_info, value) == 24);

    const _: () = assert!(::core::mem::size_of::<contract_info>() == 64);
    const _: () = assert!(::core::mem::align_of::<contract_info>() == 8);
    const _: () = assert!(::core::mem::offset_of!(contract_info, id) == 8);
    const _: () = assert!(::core::mem::offset_of!(contract_info, abi_major) == 24);
    const _: () = assert!(::core::mem::offset_of!(contract_info, abi_minor) == 28);
    const _: () = assert!(::core::mem::offset_of!(contract_info, abi_patch) == 32);
    const _: () = assert!(::core::mem::offset_of!(contract_info, schema_hash) == 40);
    const _: () = assert!(::core::mem::offset_of!(contract_info, method_count) == 56);

    #[repr(C)]
    pub struct refrain_host_metadata_vtable {
        pub struct_size: u32,
        pub abi_major: u32,
        pub abi_minor: u32,
        pub abi_patch: u32,
        pub type_count: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> u32>,
        pub type_at: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, index: u32, out_value: *mut type_info) -> RefrainStatus>,
        pub type_find: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, id: RefrainStr, out_value: *mut type_info) -> RefrainStatus>,
        pub field_at: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, type_id: RefrainStr, index: u32, out_value: *mut field_info) -> RefrainStatus>,
        pub enum_value_at: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, type_id: RefrainStr, index: u32, out_value: *mut enum_value_info) -> RefrainStatus>,
        pub contract_count: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> u32>,
        pub contract_at: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, index: u32, out_value: *mut contract_info) -> RefrainStatus>,
        pub schema_hash: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> RefrainStr>,
    }

    pub const REFRAIN_HOST_METADATA_SERVICE_ID: &str = "refrain:host.metadata";
    pub const REFRAIN_HOST_METADATA_ABI_MAJOR: u32 = 1;
    pub const REFRAIN_HOST_METADATA_ABI_MINOR: u32 = 0;
    pub const REFRAIN_HOST_METADATA_ABI_PATCH: u32 = 0;

} // pub mod metadata

pub mod platform {
    use super::*;

    pub type platform_error = u32;
    pub const PLATFORM_ERROR_INVALID_ARGUMENT: platform_error = 1;
    pub const PLATFORM_ERROR_NOT_FOUND: platform_error = 2;
    pub const PLATFORM_ERROR_PERMISSION_DENIED: platform_error = 3;
    pub const PLATFORM_ERROR_IO_ERROR: platform_error = 4;
    pub const PLATFORM_ERROR_OUT_OF_MEMORY: platform_error = 5;
    pub const PLATFORM_ERROR_UNSUPPORTED: platform_error = 6;
    pub const PLATFORM_ERROR_ALREADY_EXISTS: platform_error = 7;
    pub const PLATFORM_ERROR_READ_ONLY: platform_error = 8;
    pub const PLATFORM_ERROR_LIMIT_EXCEEDED: platform_error = 9;
    pub const PLATFORM_ERROR_INTERNAL: platform_error = 10;

    pub type file_open_flags = u32;
    pub const FILE_OPEN_FLAGS_READ_ONLY: file_open_flags = 1;
    pub const FILE_OPEN_FLAGS_READ_WRITE: file_open_flags = 2;
    pub const FILE_OPEN_FLAGS_CREATE: file_open_flags = 4;
    pub const FILE_OPEN_FLAGS_EXCLUSIVE: file_open_flags = 8;
    pub const FILE_OPEN_FLAGS_TRUNCATE: file_open_flags = 16;
    pub const FILE_OPEN_FLAGS_NO_FOLLOW: file_open_flags = 32;

    pub type file = u64;
    pub const FILE_INVALID: file = 0;

    pub type map = u64;
    pub const MAP_INVALID: map = 0;

    #[repr(C)]
    pub struct refrain_host_platform_vtable {
        pub struct_size: u32,
        pub abi_major: u32,
        pub abi_minor: u32,
        pub abi_patch: u32,
        pub file_open: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, path: RefrainStr, flags: file_open_flags, out_value: *mut file) -> RefrainStatus>,
        pub file_close: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: file) -> RefrainStatus>,
        pub file_read: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: file, offset: u64, buffer: RefrainMutBytes, out_value: *mut u64) -> RefrainStatus>,
        pub file_write: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: file, offset: u64, data: RefrainBytes, out_value: *mut u64) -> RefrainStatus>,
        pub file_size: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: file, out_value: *mut u64) -> RefrainStatus>,
        pub file_sync: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: file) -> RefrainStatus>,
        pub file_exists: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, path: RefrainStr) -> u8>,
        pub mmap_open: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, path: RefrainStr, out_value: *mut map) -> RefrainStatus>,
        pub mmap_data: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: map, out_value: *mut RefrainBytes) -> RefrainStatus>,
        pub mmap_close: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, handle: map) -> RefrainStatus>,
        pub crash_annotation: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void, key: RefrainStr, value: RefrainStr) -> ()>,
        pub temp_directory: Option<unsafe extern "C" fn(self_: *mut ::core::ffi::c_void) -> RefrainStr>,
    }

    pub const REFRAIN_HOST_PLATFORM_SERVICE_ID: &str = "refrain:host.platform";
    pub const REFRAIN_HOST_PLATFORM_ABI_MAJOR: u32 = 1;
    pub const REFRAIN_HOST_PLATFORM_ABI_MINOR: u32 = 0;
    pub const REFRAIN_HOST_PLATFORM_ABI_PATCH: u32 = 0;

} // pub mod platform
