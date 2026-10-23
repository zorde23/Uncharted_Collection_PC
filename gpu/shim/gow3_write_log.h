// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_WRITE_LOG=1 — a ring of the writes the GPU emulation makes to guest memory (fences,
// WriteData, buffer and image downloads), dumped when the guest faults: which of them landed
// where the guest's data got corrupted. Diagnostic only.
#pragma once

#include <cstdint>

namespace Gow3WriteLog {
enum Source : std::uint32_t { Backing, WriteData, Fence, FenceIntent, WriteDataIntent, EosIntent };
bool Enabled();
/// GOW3_WRITE_LOG=1: at the write itself (changes the timing of those writes).
void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source);
/// GOW3_WRITE_LOG=2: where the GPU command thread decodes a fence or WriteData, off the write
/// path (the address and value it will write later).
void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source);
/// uc3, GOW3_WRITE_LOG=1: a buffer readback about to write `size` bytes of `data` to guest
/// memory at `address`; reports any fence or WriteData the GPU emulation wrote there recently
/// (precise readbacks: labels go through the backing view, unseen by the page tracker, so a
/// readback of their range brings the GPU's copy back over them).
/// uc3: with GOW3_FAULT_WRITES=1 every download and backing write is also kept in a small ring
/// that the fault handler prints for the pages the registers point at.
void NoteDownload(std::uint64_t address, const void* data, std::uint64_t size);
bool FaultWrites();
}

extern "C" void gow3gpu_dump_guest_writes(void* ucontext);
