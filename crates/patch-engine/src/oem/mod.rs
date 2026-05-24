//! OEM-specific patches — host-only (gated behind `feature = "host"`).
//!
//! Each OEM lives in its own submodule (`oplus`, etc.); the engine
//! aggregates them by [`crate::Oem`] selector in
//! [`crate::Engine::ensure_init_scoped`].

pub mod oplus;
