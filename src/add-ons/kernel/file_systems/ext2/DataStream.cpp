/*
 * Copyright 2001-2010, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Janito V. Ferreira Filho
 */


#include "DataStream.h"

#include "CachedBlock.h"
#include "Volume.h"


//#define TRACE_EXT2
#ifdef TRACE_EXT2
#	define TRACE(x...) dprintf("\33[34mext2:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif


DataStream::DataStream(Volume* volume, ext2_data_stream* stream,
	off_t size)
	:
	kBlockSize(volume->BlockSize()),
	kIndirectsPerBlock(kBlockSize / sizeof(uint32)),
	kIndirectsPerBlock2(kIndirectsPerBlock * kIndirectsPerBlock),
	kIndirectsPerBlock3(kIndirectsPerBlock2 * kIndirectsPerBlock),
	kMaxDirect(EXT2_DIRECT_BLOCKS),
	kMaxIndirect(kMaxDirect + kIndirectsPerBlock),
	kMaxDoubleIndirect(kMaxIndirect + kIndirectsPerBlock2),
	fVolume(volume),
	fStream(stream),
	fFirstBlock(volume->FirstDataBlock()),
	fAllocated(0),
	fAllocatedPos(fFirstBlock),
	fWaiting(0),
	fFreeStart(0),
	fFreeCount(0),
	fRemovedBlocks(0)
{
	fNumBlocks = size == 0 ? 0 : (size - 1) / kBlockSize + 1;
}


DataStream::~DataStream()
{
}


status_t
DataStream::Enlarge(Transaction& transaction, uint32& numBlocks)
{
	TRACE("DataStream::Enlarge(): current size: %lu, target size: %lu\n",
		fNumBlocks, numBlocks);
	
	uint32 targetBlocks = numBlocks;
	fWaiting = _BlocksNeeded(numBlocks);
	numBlocks = fWaiting;

	status_t status;

	if (fNumBlocks <= kMaxDirect) {
		status = _AddForDirectBlocks(transaction, targetBlocks);

		if (status != B_OK)
			return status;

		TRACE("DataStream::Enlarge(): current size: %lu, target size: %lu\n",
			fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	if (fNumBlocks <= kMaxIndirect) {
		status = _AddForIndirectBlock(transaction, targetBlocks);

		if (status != B_OK)
			return status;

		TRACE("DataStream::Enlarge(): current size: %lu, target size: %lu\n",
			fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	if (fNumBlocks <= kMaxDoubleIndirect) {
		status = _AddForDoubleIndirectBlock(transaction, targetBlocks);

		if (status != B_OK)
			return status;

		TRACE("DataStream::Enlarge(): current size: %lu, target size: %lu\n",
			fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	TRACE("DataStream::Enlarge(): allocated: %lu, waiting: %lu\n", fAllocated,
		fWaiting);

	return _AddForTripleIndirectBlock(transaction, targetBlocks);
}


status_t
DataStream::Shrink(Transaction& transaction, uint32& numBlocks)
{
	TRACE("DataStream::Shrink(): current size: %lu, target size: %lu\n",
		fNumBlocks, numBlocks);

	fFreeStart = 0;
	fFreeCount = 0;
	fRemovedBlocks = 0;

	uint32 oldNumBlocks = fNumBlocks;
	uint32 blocksToRemove = fNumBlocks - numBlocks;

	status_t status;

	if (numBlocks < kMaxDirect) {
		status = _RemoveFromDirectBlocks(transaction, numBlocks);

		if (status != B_OK)
			return status;

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;
			numBlocks = _BlocksNeeded(oldNumBlocks);

			return _PerformFree(transaction);
		}
	}

	if (numBlocks < kMaxIndirect) {
		status = _RemoveFromIndirectBlock(transaction, numBlocks);

		if (status != B_OK)
			return status;

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;
			numBlocks = _BlocksNeeded(oldNumBlocks);

			return _PerformFree(transaction);
		}
	}

	if (numBlocks < kMaxDoubleIndirect) {
		status = _RemoveFromDoubleIndirectBlock(transaction, numBlocks);

		if (status != B_OK)
			return status;

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;
			numBlocks = _BlocksNeeded(oldNumBlocks);

			return _PerformFree(transaction);
		}
	}

	status = _RemoveFromTripleIndirectBlock(transaction, numBlocks);

	if (status != B_OK)
		return status;

	fNumBlocks -= fRemovedBlocks;
	numBlocks = _BlocksNeeded(oldNumBlocks);

	return _PerformFree(transaction);
}


uint32
DataStream::_BlocksNeeded(uint32 numBlocks)
{
	TRACE("DataStream::BlocksNeeded(): num blocks %lu\n", numBlocks);
	uint32 blocksNeeded = 0;

	if (numBlocks > fNumBlocks) {
		blocksNeeded += numBlocks - fNumBlocks;

		if (numBlocks > kMaxDirect) {
			if (fNumBlocks <= kMaxDirect)
				blocksNeeded += 1;

			if (numBlocks > kMaxIndirect) {
				if (fNumBlocks <= kMaxIndirect) {
					blocksNeeded += 2 + (numBlocks - kMaxIndirect - 1)
						/ kIndirectsPerBlock;
				} else {
					blocksNeeded += (numBlocks - kMaxIndirect - 1)
						/ kIndirectsPerBlock - (fNumBlocks
							- kMaxIndirect - 1) / kIndirectsPerBlock;
				}

				if (numBlocks > kMaxDoubleIndirect) {
					if (fNumBlocks <= kMaxDoubleIndirect) {
						blocksNeeded += 2 + (numBlocks - kMaxDoubleIndirect - 1)
							/ kIndirectsPerBlock2;
					} else {
						blocksNeeded += (numBlocks - kMaxDoubleIndirect - 1)
							/ kIndirectsPerBlock - (fNumBlocks 
								- kMaxDoubleIndirect - 1) / kIndirectsPerBlock;
					}
				}
			}
		}
	}

	TRACE("DataStream::BlocksNeeded(): %lu\n", blocksNeeded);
	return blocksNeeded;
}


status_t
DataStream::_GetBlock(Transaction& transaction, uint32& block)
{
	TRACE("DataStream::_GetBlock(): allocated: %lu, pos: %lu, waiting: %lu\n",
		fAllocated, fAllocatedPos, fWaiting);

	if (fAllocated == 0) {
		uint32 blockGroup = (fAllocatedPos - fFirstBlock)
			/ fVolume->BlocksPerGroup();
		fAllocatedPos %= fVolume->BlocksPerGroup();

		status_t status = fVolume->AllocateBlocks(transaction, 1, fWaiting,
			blockGroup, fAllocatedPos, fAllocated);
		if (status != B_OK)
			return status;

		fWaiting -= fAllocated;
		fAllocatedPos += fVolume->BlocksPerGroup() * blockGroup + fFirstBlock;

		TRACE("DataStream::_GetBlock(): newAllocated: %lu, newpos: %lu,"
			"newwaiting: %lu\n", fAllocated, fAllocatedPos, fWaiting);
	}

	fAllocated--;
	block = fAllocatedPos++;

	return B_OK;
}


status_t
DataStream::_PrepareBlock(Transaction& transaction, uint32* pos,
	uint32& blockNum, bool& clear)
{
	blockNum = B_LENDIAN_TO_HOST_INT32(*pos);
	clear = false;

	if (blockNum == 0) {
		status_t status = _GetBlock(transaction, blockNum);
		if (status != B_OK)
			return status;

		*pos = B_HOST_TO_LENDIAN_INT32(blockNum);
		clear = true;
	}

	return B_OK;
}


status_t
DataStream::_AddBlocks(Transaction& transaction, uint32* block, uint32 _count)
{
	uint32 count = _count;
	TRACE("DataStream::_AddBlocks(): count: %lu\n", count);

	while (count > 0) {
		uint32 blockNum;
		status_t status = _GetBlock(transaction, blockNum);
		if (status != B_OK)
			return status;

		*(block++) = B_HOST_TO_LENDIAN_INT32(blockNum);
		--count;
	}

	fNumBlocks += _count;

	return B_OK;
}


status_t
DataStream::_AddBlocks(Transaction& transaction, uint32* block, uint32 start,
	uint32 end, int recursion)
{
	TRACE("DataStream::_AddBlocks(): start: %lu, end %lu, recursion: %d\n",
		start, end, recursion);

	bool clear;
	uint32 blockNum;
	status_t status = _PrepareBlock(transaction, block, blockNum, clear);
	if (status != B_OK)
		return status;

	CachedBlock cached(fVolume);	
	uint32* childBlock = (uint32*)cached.SetToWritable(transaction, blockNum,
		clear);
	if (childBlock == NULL)
		return B_IO_ERROR;

	if (recursion == 0)
		return _AddBlocks(transaction, &childBlock[start], end - start);

	uint32 elementWidth;
	if (recursion == 1)
		elementWidth = kIndirectsPerBlock;
	else if (recursion == 2)
		elementWidth = kIndirectsPerBlock2;
	else {
		panic("Undefined recursion level\n");
		elementWidth = 0;
	}

	uint32 elementPos = start / elementWidth;
	uint32 endPos = end / elementWidth;

	TRACE("DataStream::_AddBlocks(): element pos: %lu, end pos: %lu\n",
		elementPos, endPos);

	recursion--;

	if (elementPos == endPos) {
		return _AddBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, end % elementWidth, recursion);
	}
	
	if (start % elementWidth != 0) {
		status = _AddBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, elementWidth, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	while (elementPos < endPos) {
		status = _AddBlocks(transaction, &childBlock[elementPos], 0,
			elementWidth, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	if (end % elementWidth != 0) {
		status = _AddBlocks(transaction, &childBlock[elementPos], 0,
			end % elementWidth, recursion);
		if (status != B_OK)
			return status;
	}
		
	return B_OK;
}


status_t
DataStream::_AddForDirectBlocks(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_AddForDirectBlocks(): current size: %lu, target size: "
		"%lu\n", fNumBlocks, numBlocks);
	uint32* direct = &fStream->direct[fNumBlocks];
	uint32 end = numBlocks > kMaxDirect ? kMaxDirect : numBlocks;

	return _AddBlocks(transaction, direct, end - fNumBlocks);
}


status_t
DataStream::_AddForIndirectBlock(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_AddForIndirectBlocks(): current size: %lu, target "
		"size: %lu\n", fNumBlocks, numBlocks);
	uint32 *indirect = &fStream->indirect;
	uint32 start = fNumBlocks - kMaxDirect;
	uint32 end = numBlocks - kMaxDirect;

	if (end > kIndirectsPerBlock)
		end = kIndirectsPerBlock;

	return _AddBlocks(transaction, indirect, start, end, 0);
}


status_t
DataStream::_AddForDoubleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_AddForDoubleIndirectBlock(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32 *doubleIndirect = &fStream->double_indirect;
	uint32 start = fNumBlocks - kMaxIndirect;
	uint32 end = numBlocks - kMaxIndirect;

	if (end > kIndirectsPerBlock2)
		end = kIndirectsPerBlock2;

	return _AddBlocks(transaction, doubleIndirect, start, end, 1);
}


status_t
DataStream::_AddForTripleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_AddForTripleIndirectBlock(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32 *tripleIndirect = &fStream->triple_indirect;
	uint32 start = fNumBlocks - kMaxDoubleIndirect;
	uint32 end = numBlocks - kMaxDoubleIndirect;

	return _AddBlocks(transaction, tripleIndirect, start, end, 2);
}


status_t
DataStream::_PerformFree(Transaction& transaction)
{
	TRACE("DataStream::_PerformFree(): start: %lu, count: %lu\n", fFreeStart,
		fFreeCount);
	status_t status;

	if (fFreeCount == 0)
		status = B_OK;
	else
		status = fVolume->FreeBlocks(transaction, fFreeStart, fFreeCount);

	fFreeStart = 0;
	fFreeCount = 0;

	return status;
}


status_t
DataStream::_MarkBlockForRemoval(Transaction& transaction, uint32* block)
{
	TRACE("DataStream::_MarkBlockForRemoval(*(%p) = %lu): free start: %lu, "
		"free count: %lu\n", block, *block, fFreeStart, fFreeCount);
	uint32 blockNum = B_LENDIAN_TO_HOST_INT32(*block);	
	*block = 0;

	if (blockNum != fFreeStart + fFreeCount) {
		if (fFreeCount != 0) {
			status_t status = fVolume->FreeBlocks(transaction, fFreeStart,
				fFreeCount);
			if (status != B_OK)
				return status;
		}

		fFreeStart = blockNum;
		fFreeCount = 0;
	}

	fFreeCount++;

	return B_OK;
}


status_t
DataStream::_FreeBlocks(Transaction& transaction, uint32* block, uint32 _count)
{
	uint32 count = _count;
	TRACE("DataStream::_FreeBlocks(%p, %lu)\n", block, count);

	while (count > 0) {
		status_t status = _MarkBlockForRemoval(transaction, block);
		if (status != B_OK)
			return status;

		block++;
		count--;
	}

	fRemovedBlocks += _count;

	return B_OK;
}


status_t
DataStream::_FreeBlocks(Transaction& transaction, uint32* block, uint32 start,
	uint32 end, bool freeParent, int recursion)
{
	// TODO: Designed specifically for shrinking. Perhaps make it more general?
	TRACE("DataStream::_FreeBlocks(%p, %lu, %lu, %c, %d)\n",
		block, start, end, freeParent ? 't' : 'f', recursion);

	uint32 blockNum = B_LENDIAN_TO_HOST_INT32(*block);

	if (freeParent) {
		status_t status = _MarkBlockForRemoval(transaction, block);
		if (status != B_OK)
			return status;
	}

	CachedBlock cached(fVolume);
	uint32* childBlock = (uint32*)cached.SetToWritable(transaction, blockNum);
	if (childBlock == NULL)
		return B_IO_ERROR;

	if (recursion == 0)
		return _FreeBlocks(transaction, &childBlock[start], end - start);

	uint32 elementWidth;
	if (recursion == 1)
		elementWidth = kIndirectsPerBlock;
	else if (recursion == 2)
		elementWidth = kIndirectsPerBlock2;
	else {
		panic("Undefinied recursion level\n");
		elementWidth = 0;
	}

	uint32 elementPos = start / elementWidth;
	uint32 endPos = end / elementWidth;

	recursion--;

	if (elementPos == endPos) {
		bool free = freeParent || start % elementWidth == 0;
		return _FreeBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, end % elementWidth, free, recursion);
	}

	status_t status = B_OK;

	if (start % elementWidth != 0) {
		status = _FreeBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, elementWidth, false, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	while (elementPos < endPos) {
		status = _FreeBlocks(transaction, &childBlock[elementPos], 0,
			elementWidth, true, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	if (end % elementWidth != 0) {
		status = _FreeBlocks(transaction, &childBlock[elementPos], 0,
			end % elementWidth, true, recursion);
	}

	return status;
}


status_t
DataStream::_RemoveFromDirectBlocks(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromDirectBlocks(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32* direct = &fStream->direct[numBlocks];
	uint32 end = fNumBlocks > kMaxDirect ? kMaxDirect : fNumBlocks;

	return _FreeBlocks(transaction, direct, end - numBlocks);
}


status_t
DataStream::_RemoveFromIndirectBlock(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromIndirectBlock(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32* indirect = &fStream->indirect;
	uint32 start = numBlocks <= kMaxDirect ? 0 : numBlocks - kMaxDirect;
	uint32 end = fNumBlocks - kMaxDirect;

	if (end > kIndirectsPerBlock)
		end = kIndirectsPerBlock;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, indirect, start, end, freeAll, 0);
}


status_t
DataStream::_RemoveFromDoubleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromDoubleIndirectBlock(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32* doubleIndirect = &fStream->double_indirect;
	uint32 start = numBlocks <= kMaxIndirect ? 0 : numBlocks - kMaxIndirect;
	uint32 end = fNumBlocks - kMaxIndirect;

	if (end > kIndirectsPerBlock2)
		end = kIndirectsPerBlock2;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, doubleIndirect, start, end, freeAll, 1);
}


status_t
DataStream::_RemoveFromTripleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromTripleIndirectBlock(): current size: %lu, "
		"target size: %lu\n", fNumBlocks, numBlocks);
	uint32* tripleIndirect = &fStream->triple_indirect;
	uint32 start = numBlocks <= kMaxDoubleIndirect ? 0
		: numBlocks - kMaxDoubleIndirect;
	uint32 end = fNumBlocks - kMaxDoubleIndirect;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, tripleIndirect, start, end, freeAll, 2);
}