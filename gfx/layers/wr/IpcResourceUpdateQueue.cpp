/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IpcResourceUpdateQueue.h"
#include <string.h>
#include <algorithm>
#include "mozilla/Maybe.h"
#include "mozilla/ipc/SharedMemory.h"
#include "mozilla/layers/WebRenderBridgeChild.h"

namespace mozilla {
namespace wr {

using namespace mozilla::layers;

ShmSegmentsWriter::ShmSegmentsWriter(layers::WebRenderBridgeChild* aAllocator, size_t aChunkSize)
: mShmAllocator(aAllocator)
, mCursor(0)
, mChunkSize(aChunkSize)
{
  MOZ_ASSERT(mShmAllocator);
}

ShmSegmentsWriter::~ShmSegmentsWriter()
{
  Clear();
}

layers::OffsetRange
ShmSegmentsWriter::Write(Range<uint8_t> aBytes)
{
  const size_t start = mCursor;
  const size_t length = aBytes.length();

  if (length >= mChunkSize * 4) {
    auto range = AllocLargeChunk(length);
    if (range.length()) {
      // Allocation was successful
      uint8_t* dstPtr = mLargeAllocs.LastElement().get<uint8_t>();
      memcpy(dstPtr, aBytes.begin().get(), length);
    }
    return range;
  }

  int remainingBytesToCopy = length;

  size_t srcCursor = 0;
  size_t dstCursor = mCursor;
  size_t currAllocLen = mSmallAllocs.Length();

  while (remainingBytesToCopy > 0) {
    if (dstCursor >= mSmallAllocs.Length() * mChunkSize) {
      if (!AllocChunk()) {
        // Allocation failed, so roll back to the state at the start of this
        // Write() call and abort.
        for (size_t i = mSmallAllocs.Length() ; currAllocLen < i ; i--) {
          MOZ_ASSERT(i > 0);
          RefCountedShmem& shm = mSmallAllocs.ElementAt(i - 1);
          RefCountedShm::Dealloc(mShmAllocator, shm);
          mSmallAllocs.RemoveElementAt(i - 1);
        }
        MOZ_ASSERT(mSmallAllocs.Length() == currAllocLen);
        return layers::OffsetRange(0, start, 0);
      }
      // Allocation succeeded, so dstCursor should now be pointing to
      // something inside the allocation buffer
      MOZ_ASSERT(dstCursor < (mSmallAllocs.Length() * mChunkSize));
    }

    const size_t dstMaxOffset = mChunkSize * mSmallAllocs.Length();
    const size_t dstBaseOffset = mChunkSize * (mSmallAllocs.Length() - 1);

    MOZ_ASSERT(dstCursor >= dstBaseOffset);
    MOZ_ASSERT(dstCursor <= dstMaxOffset);

    size_t availableRange = dstMaxOffset - dstCursor;
    size_t copyRange = std::min<int>(availableRange, remainingBytesToCopy);

    uint8_t* srcPtr = &aBytes[srcCursor];
    uint8_t* dstPtr = RefCountedShm::GetBytes(mSmallAllocs.LastElement()) + (dstCursor - dstBaseOffset);

    memcpy(dstPtr, srcPtr, copyRange);

    srcCursor += copyRange;
    dstCursor += copyRange;
    remainingBytesToCopy -= copyRange;

    // sanity check
    MOZ_ASSERT(remainingBytesToCopy >= 0);
  }

  mCursor += length;

  return layers::OffsetRange(0, start, length);
}

bool
ShmSegmentsWriter::AllocChunk()
{
  RefCountedShmem shm;
  if (!mShmAllocator->AllocResourceShmem(mChunkSize, shm)) {
    gfxCriticalNote << "ShmSegmentsWriter failed to allocate chunk #" << mSmallAllocs.Length();
    MOZ_ASSERT(false, "ShmSegmentsWriter fails to allocate chunk");
    return false;
  }
  RefCountedShm::AddRef(shm);
  mSmallAllocs.AppendElement(shm);
  return true;
}

layers::OffsetRange
ShmSegmentsWriter::AllocLargeChunk(size_t aSize)
{
  ipc::Shmem shm;
  auto shmType = ipc::SharedMemory::SharedMemoryType::TYPE_BASIC;
  if (!mShmAllocator->AllocShmem(aSize, shmType, &shm)) {
    gfxCriticalNote << "ShmSegmentsWriter failed to allocate large chunk of size " << aSize;
    MOZ_ASSERT(false, "ShmSegmentsWriter fails to allocate large chunk");
    return layers::OffsetRange(0, 0, 0);
  }
  mLargeAllocs.AppendElement(shm);

  return layers::OffsetRange(mLargeAllocs.Length(), 0, aSize);
}

void
ShmSegmentsWriter::Flush(nsTArray<RefCountedShmem>& aSmallAllocs, nsTArray<ipc::Shmem>& aLargeAllocs)
{
  MOZ_ASSERT(aSmallAllocs.IsEmpty());
  MOZ_ASSERT(aLargeAllocs.IsEmpty());
  mSmallAllocs.SwapElements(aSmallAllocs);
  mLargeAllocs.SwapElements(aLargeAllocs);
  mCursor = 0;
}

bool
ShmSegmentsWriter::IsEmpty() const
{
  return mCursor == 0;
}

void
ShmSegmentsWriter::Clear()
{
  if (mShmAllocator) {
    IpcResourceUpdateQueue::ReleaseShmems(mShmAllocator, mSmallAllocs);
    IpcResourceUpdateQueue::ReleaseShmems(mShmAllocator, mLargeAllocs);
  }
  mCursor = 0;
}

ShmSegmentsReader::ShmSegmentsReader(const nsTArray<RefCountedShmem>& aSmallShmems,
                                     const nsTArray<ipc::Shmem>& aLargeShmems)
: mSmallAllocs(aSmallShmems)
, mLargeAllocs(aLargeShmems)
, mChunkSize(0)
{
  if (mSmallAllocs.IsEmpty()) {
    return;
  }

  mChunkSize = RefCountedShm::GetSize(mSmallAllocs[0]);

  // Check that all shmems are readable and have the same size. If anything
  // isn't right, set mChunkSize to zero which signifies that the reader is
  // in an invalid state and Read calls will return false;
  for (const auto& shm : mSmallAllocs) {
    if (!RefCountedShm::IsValid(shm)
        || RefCountedShm::GetSize(shm) != mChunkSize
        || RefCountedShm::GetBytes(shm) == nullptr) {
      mChunkSize = 0;
      return;
    }
  }

  for (const auto& shm : mLargeAllocs) {
    if (!shm.IsReadable()
        || shm.get<uint8_t>() == nullptr) {
      mChunkSize = 0;
      return;
    }
  }
}

bool
ShmSegmentsReader::ReadLarge(const layers::OffsetRange& aRange, wr::Vec<uint8_t>& aInto)
{
  // source = zero is for small allocs.
  MOZ_RELEASE_ASSERT(aRange.source() != 0);
  if (aRange.source() > mLargeAllocs.Length()) {
    return false;
  }
  size_t id = aRange.source() - 1;
  const ipc::Shmem& shm = mLargeAllocs[id];
  if (shm.Size<uint8_t>() < aRange.length()) {
    return false;
  }

  uint8_t* srcPtr = shm.get<uint8_t>();
  aInto.PushBytes(Range<uint8_t>(srcPtr, aRange.length()));

  return true;
}

bool
ShmSegmentsReader::Read(const layers::OffsetRange& aRange, wr::Vec<uint8_t>& aInto)
{
  if (aRange.length() == 0) {
    return true;
  }

  if (aRange.source() != 0) {
    return ReadLarge(aRange, aInto);
  }

  if (mChunkSize == 0) {
    return false;
  }

  if (aRange.start() + aRange.length() > mChunkSize * mSmallAllocs.Length()) {
    return false;
  }

  size_t initialLength = aInto.Length();

  size_t srcCursor = aRange.start();
  int remainingBytesToCopy = aRange.length();
  while (remainingBytesToCopy > 0) {
    const size_t shm_idx = srcCursor / mChunkSize;
    const size_t ptrOffset = srcCursor % mChunkSize;
    const size_t copyRange = std::min<int>(remainingBytesToCopy, mChunkSize - ptrOffset);
    uint8_t* srcPtr = RefCountedShm::GetBytes(mSmallAllocs[shm_idx]) + ptrOffset;

    aInto.PushBytes(Range<uint8_t>(srcPtr, copyRange));

    srcCursor += copyRange;
    remainingBytesToCopy -= copyRange;
  }

  return aInto.Length() - initialLength == aRange.length();
}

IpcResourceUpdateQueue::IpcResourceUpdateQueue(layers::WebRenderBridgeChild* aAllocator,
                                               size_t aChunkSize)
: mWriter(Move(aAllocator), aChunkSize)
{}

bool
IpcResourceUpdateQueue::AddImage(ImageKey key, const ImageDescriptor& aDescriptor,
                                 Range<uint8_t> aBytes)
{
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpAddImage(aDescriptor, bytes, 0, key));
  return true;
}

bool
IpcResourceUpdateQueue::AddBlobImage(ImageKey key, const ImageDescriptor& aDescriptor,
                                     Range<uint8_t> aBytes)
{
  MOZ_RELEASE_ASSERT(aDescriptor.width > 0 && aDescriptor.height > 0);
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpAddBlobImage(aDescriptor, bytes, 0, key));
  return true;
}

void
IpcResourceUpdateQueue::AddExternalImage(wr::ExternalImageId aExtId, wr::ImageKey aKey)
{
  mUpdates.AppendElement(layers::OpAddExternalImage(aExtId, aKey));
}

bool
IpcResourceUpdateQueue::UpdateImageBuffer(ImageKey aKey,
                                          const ImageDescriptor& aDescriptor,
                                          Range<uint8_t> aBytes)
{
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpUpdateImage(aDescriptor, bytes, aKey));
  return true;
}

bool
IpcResourceUpdateQueue::UpdateBlobImage(ImageKey aKey,
                                        const ImageDescriptor& aDescriptor,
                                        Range<uint8_t> aBytes,
                                        ImageIntRect aDirtyRect)
{
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpUpdateBlobImage(aDescriptor, bytes, aKey, aDirtyRect));
  return true;
}

void
IpcResourceUpdateQueue::DeleteImage(ImageKey aKey)
{
  mUpdates.AppendElement(layers::OpDeleteImage(aKey));
}

bool
IpcResourceUpdateQueue::AddRawFont(wr::FontKey aKey, Range<uint8_t> aBytes, uint32_t aIndex)
{
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpAddRawFont(bytes, aIndex, aKey));
  return true;
}

bool
IpcResourceUpdateQueue::AddFontDescriptor(wr::FontKey aKey, Range<uint8_t> aBytes, uint32_t aIndex)
{
  auto bytes = mWriter.Write(aBytes);
  if (!bytes.length()) {
    return false;
  }
  mUpdates.AppendElement(layers::OpAddFontDescriptor(bytes, aIndex, aKey));
  return true;
}

void
IpcResourceUpdateQueue::DeleteFont(wr::FontKey aKey)
{
  mUpdates.AppendElement(layers::OpDeleteFont(aKey));
}

void
IpcResourceUpdateQueue::AddFontInstance(wr::FontInstanceKey aKey,
                                        wr::FontKey aFontKey,
                                        float aGlyphSize,
                                        const wr::FontInstanceOptions* aOptions,
                                        const wr::FontInstancePlatformOptions* aPlatformOptions,
                                        Range<const gfx::FontVariation> aVariations)
{
  auto bytes = mWriter.WriteAsBytes(aVariations);
  mUpdates.AppendElement(layers::OpAddFontInstance(
    aOptions ? Some(*aOptions) : Nothing(),
    aPlatformOptions ? Some(*aPlatformOptions) : Nothing(),
    bytes,
    aKey, aFontKey,
    aGlyphSize
  ));
}

void
IpcResourceUpdateQueue::DeleteFontInstance(wr::FontInstanceKey aKey)
{
  mUpdates.AppendElement(layers::OpDeleteFontInstance(aKey));
}

void
IpcResourceUpdateQueue::Flush(nsTArray<layers::OpUpdateResource>& aUpdates,
                              nsTArray<layers::RefCountedShmem>& aSmallAllocs,
                              nsTArray<ipc::Shmem>& aLargeAllocs)
{
  aUpdates.Clear();
  mUpdates.SwapElements(aUpdates);
  mWriter.Flush(aSmallAllocs, aLargeAllocs);
}

bool
IpcResourceUpdateQueue::IsEmpty() const
{
  if (mUpdates.Length() == 0) {
    MOZ_ASSERT(mWriter.IsEmpty());
    return true;
  }
  return false;
}

void
IpcResourceUpdateQueue::Clear()
{
  mWriter.Clear();
  mUpdates.Clear();
}

//static
void
IpcResourceUpdateQueue::ReleaseShmems(ipc::IProtocol* aShmAllocator, nsTArray<layers::RefCountedShmem>& aShms)
{
  for (auto& shm : aShms) {
    if (RefCountedShm::IsValid(shm) && RefCountedShm::Release(shm) == 0) {
      RefCountedShm::Dealloc(aShmAllocator, shm);
    }
  }
  aShms.Clear();
}

//static
void
IpcResourceUpdateQueue::ReleaseShmems(ipc::IProtocol* aShmAllocator, nsTArray<ipc::Shmem>& aShms)
{
  for (auto& shm : aShms) {
    aShmAllocator->DeallocShmem(shm);
  }
  aShms.Clear();
}

} // namespace
} // namespace
