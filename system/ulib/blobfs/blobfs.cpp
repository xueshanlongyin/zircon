// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <inttypes.h>
#include <limits>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <blobfs/blobfs.h>
#include <blobfs/extent-reserver.h>
#include <blobfs/iterator/allocated-extent-iterator.h>
#include <blobfs/iterator/block-iterator.h>
#include <blobfs/iterator/extent-iterator.h>
#include <blobfs/iterator/node-populator.h>
#include <blobfs/iterator/vector-extent-iterator.h>
#include <blobfs/lz4.h>
#include <blobfs/node-reserver.h>
#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <fbl/auto_call.h>
#include <fbl/ref_ptr.h>
#include <fs/block-txn.h>
#include <fs/ticker.h>
#include <lib/async/cpp/task.h>
#include <lib/fdio/debug.h>
#include <lib/zx/event.h>
#include <zircon/compiler.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#define ZXDEBUG 0

#include <utility>

using digest::Digest;
using digest::MerkleTree;

namespace blobfs {
namespace {

zx_status_t CheckFvmConsistency(const Superblock* info, int block_fd) {
    if ((info->flags & kBlobFlagFVM) == 0) {
        return ZX_OK;
    }

    fvm_info_t fvm_info;
    zx_status_t status = static_cast<zx_status_t>(ioctl_block_fvm_query(block_fd, &fvm_info));
    if (status < ZX_OK) {
        FS_TRACE_ERROR("blobfs: Unable to query FVM, fd: %d status: 0x%x\n", block_fd, status);
        return ZX_ERR_UNAVAILABLE;
    }

    if (info->slice_size != fvm_info.slice_size) {
        FS_TRACE_ERROR("blobfs: Slice size did not match expected\n");
        return ZX_ERR_BAD_STATE;
    }
    const size_t kBlocksPerSlice = info->slice_size / kBlobfsBlockSize;

    size_t expected_count[4];
    expected_count[0] = info->abm_slices;
    expected_count[1] = info->ino_slices;
    expected_count[2] = info->journal_slices;
    expected_count[3] = info->dat_slices;

    query_request_t request;
    request.count = 4;
    request.vslice_start[0] = kFVMBlockMapStart / kBlocksPerSlice;
    request.vslice_start[1] = kFVMNodeMapStart / kBlocksPerSlice;
    request.vslice_start[2] = kFVMJournalStart / kBlocksPerSlice;
    request.vslice_start[3] = kFVMDataStart / kBlocksPerSlice;

    query_response_t response;
    status = static_cast<zx_status_t>(ioctl_block_fvm_vslice_query(block_fd, &request, &response));
    if (status < ZX_OK) {
        FS_TRACE_ERROR("blobfs: Unable to query slices, status: 0x%x\n", status);
        return ZX_ERR_UNAVAILABLE;
    }

    if (response.count != request.count) {
        FS_TRACE_ERROR("blobfs: Missing slice\n");
        return ZX_ERR_BAD_STATE;
    }

    for (size_t i = 0; i < request.count; i++) {
        size_t blobfs_count = expected_count[i];
        size_t fvm_count = response.vslice_range[i].count;

        if (!response.vslice_range[i].allocated || fvm_count < blobfs_count) {
            // Currently, since Blobfs can only grow new slices, it should not be possible for
            // the FVM to report a slice size smaller than what is reported by Blobfs. In this
            // case, automatically fail without trying to resolve the situation, as it is
            // possible that Blobfs structures are allocated in the slices that have been lost.
            FS_TRACE_ERROR("blobfs: Mismatched slice count\n");
            return ZX_ERR_IO_DATA_INTEGRITY;
        }

        if (fvm_count > blobfs_count) {
            // If FVM reports more slices than we expect, try to free remainder.
            extend_request_t shrink;
            shrink.length = fvm_count - blobfs_count;
            shrink.offset = request.vslice_start[i] + blobfs_count;
            ssize_t r;
            if ((r = ioctl_block_fvm_shrink(block_fd, &shrink)) != ZX_OK) {
                FS_TRACE_ERROR("blobfs: Unable to shrink to expected size, status: %zd\n", r);
                return ZX_ERR_IO_DATA_INTEGRITY;
            }
        }
    }

    return ZX_OK;
}

// A wrapper around "Enqueue" for content which risks being larger
// than the writeback buffer.
//
// For content which is smaller than 3/4 the size of the writeback buffer: the
// content is enqueued to |work| without flushing.
//
// For content which is larger than 3/4 the size of the writeback buffer: flush
// the data by enqueueing it to the writeback thread in chunks until the
// remainder is small enough to comfortably fit within the writeback buffer.
zx_status_t EnqueuePaginated(fbl::unique_ptr<WritebackWork>* work, Blobfs* blobfs, VnodeBlob* vn,
                             const zx::vmo& vmo, uint64_t relative_block, uint64_t absolute_block,
                             uint64_t nblocks) {
    const size_t kMaxChunkBlocks = (3 * blobfs->WritebackCapacity()) / 4;
    uint64_t delta_blocks = fbl::min(nblocks, kMaxChunkBlocks);
    while (nblocks > 0) {
        (*work)->Enqueue(vmo, relative_block, absolute_block, delta_blocks);
        relative_block += delta_blocks;
        absolute_block += delta_blocks;
        nblocks -= delta_blocks;
        delta_blocks = fbl::min(nblocks, kMaxChunkBlocks);
        if (nblocks) {
            fbl::unique_ptr<WritebackWork> tmp;
            zx_status_t status = blobfs->CreateWork(&tmp, vn);
            if (status != ZX_OK) {
                return status;
            }
            if ((status = blobfs->EnqueueWork(std::move(*work), EnqueueType::kData)) != ZX_OK) {
                return status;
            }
            *work = std::move(tmp);
        }
    }
    return ZX_OK;
}

} // namespace

zx_status_t VnodeBlob::Verify() const {
    TRACE_DURATION("blobfs", "Blobfs::Verify");
    fs::Ticker ticker(blobfs_->CollectingMetrics());

    const void* data = inode_.blob_size ? GetData() : nullptr;
    const void* tree = inode_.blob_size ? GetMerkle() : nullptr;
    const uint64_t data_size = inode_.blob_size;
    const uint64_t merkle_size = MerkleTree::GetTreeLength(data_size);
    // TODO(smklein): We could lazily verify more of the VMO if
    // we could fault in pages on-demand.
    //
    // For now, we aggressively verify the entire VMO up front.
    Digest digest;
    digest = reinterpret_cast<const uint8_t*>(&digest_[0]);
    zx_status_t status = MerkleTree::Verify(data, data_size, tree,
                                            merkle_size, 0, data_size, digest);
    blobfs_->UpdateMerkleVerifyMetrics(data_size, merkle_size, ticker.End());

    if (status != ZX_OK) {
        char name[Digest::kLength * 2 + 1];
        ZX_ASSERT(digest.ToString(name, sizeof(name)) == ZX_OK);
        FS_TRACE_ERROR("blobfs verify(%s) Failure: %s\n", name, zx_status_get_string(status));
    }

    return status;
}

zx_status_t VnodeBlob::InitVmos() {
    TRACE_DURATION("blobfs", "Blobfs::InitVmos");

    if (mapping_.vmo()) {
        return ZX_OK;
    }

    uint64_t data_blocks = BlobDataBlocks(inode_);
    uint64_t merkle_blocks = MerkleTreeBlocks(inode_);
    uint64_t num_blocks = data_blocks + merkle_blocks;

    if (num_blocks == 0) {
        // No need to initialize VMO for null blob.
        return ZX_OK;
    }

    // Reverts blob back to uninitialized state on error.
    auto cleanup = fbl::MakeAutoCall([this]() { BlobCloseHandles(); });

    size_t vmo_size;
    if (mul_overflow(num_blocks, kBlobfsBlockSize, &vmo_size)) {
        FS_TRACE_ERROR("Multiplication overflow");
        return ZX_ERR_OUT_OF_RANGE;
    }

    zx_status_t status = mapping_.CreateAndMap(vmo_size, "blob");
    if (status != ZX_OK) {
        FS_TRACE_ERROR("Failed to initialize vmo; error: %d\n", status);
        return status;
    }
    if ((status = blobfs_->AttachVmo(mapping_.vmo(), &vmoid_)) != ZX_OK) {
        FS_TRACE_ERROR("Failed to attach VMO to block device; error: %d\n", status);
        return status;
    }

    if ((inode_.header.flags & kBlobFlagLZ4Compressed) != 0) {
        if ((status = InitCompressed()) != ZX_OK) {
            return status;
        }
    } else {
        if ((status = InitUncompressed()) != ZX_OK) {
            return status;
        }
    }
    if ((status = Verify()) != ZX_OK) {
        return status;
    }

    cleanup.cancel();
    return ZX_OK;
}

zx_status_t VnodeBlob::InitCompressed() {
    TRACE_DURATION("blobfs", "Blobfs::InitCompressed", "size", inode_.blob_size,
                   "blocks", inode_.block_count);
    fs::Ticker ticker(blobfs_->CollectingMetrics());
    fs::ReadTxn txn(blobfs_);
    uint32_t merkle_blocks = MerkleTreeBlocks(inode_);

    fzl::OwnedVmoMapper compressed_mapper;
    uint32_t compressed_blocks = (inode_.block_count - merkle_blocks);
    size_t compressed_size;
    if (mul_overflow(compressed_blocks, kBlobfsBlockSize, &compressed_size)) {
        FS_TRACE_ERROR("Multiplication overflow\n");
        return ZX_ERR_OUT_OF_RANGE;
    }
    zx_status_t status = compressed_mapper.CreateAndMap(compressed_size, "compressed-blob");
    if (status != ZX_OK) {
        FS_TRACE_ERROR("Failed to initialized compressed vmo; error: %d\n", status);
        return status;
    }
    vmoid_t compressed_vmoid;
    status = blobfs_->AttachVmo(compressed_mapper.vmo(), &compressed_vmoid);
    if (status != ZX_OK) {
        FS_TRACE_ERROR("Failed to attach commpressed VMO to blkdev: %d\n", status);
        return status;
    }

    auto detach = fbl::MakeAutoCall([this, &compressed_vmoid]() {
        blobfs_->DetachVmo(compressed_vmoid);
    });

    const uint64_t kDataStart = DataStartBlock(blobfs_->info_);
    AllocatedExtentIterator extent_iter(blobfs_->allocator_.get(), GetMapIndex());
    BlockIterator block_iter(&extent_iter);

    // Read the uncompressed merkle tree into the start of the blob's VMO.
    status = StreamBlocks(&block_iter, merkle_blocks,
        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
            txn.Enqueue(vmoid_, vmo_offset, dev_offset + kDataStart, length);
            return ZX_OK;
        });
    if (status != ZX_OK) {
        return status;
    }

    // Read the compressed blocks into the compressed VMO, accounting for the merkle blocks
    // which have already been seen.
    ZX_DEBUG_ASSERT(block_iter.BlockIndex() == merkle_blocks);

    status = StreamBlocks(&block_iter, compressed_blocks,
        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
            txn.Enqueue(compressed_vmoid, vmo_offset - merkle_blocks, dev_offset + kDataStart,
                        length);
            return ZX_OK;
        });

    if (status != ZX_OK) {
        return status;
    }

    if ((status = txn.Transact()) != ZX_OK) {
        FS_TRACE_ERROR("Failed to flush read transaction: %d\n", status);
        return status;
    }

    fs::Duration read_time = ticker.End();
    ticker.Reset();

    // Decompress the compressed data into the target buffer.
    size_t target_size = inode_.blob_size;
    status = Decompressor::Decompress(GetData(), &target_size,
                                      compressed_mapper.start(), &compressed_size);
    if (status != ZX_OK) {
        FS_TRACE_ERROR("Failed to decompress data: %d\n", status);
        return status;
    } else if (target_size != inode_.blob_size) {
        FS_TRACE_ERROR("Failed to fully decompress blob (%zu of %zu expected)\n",
                       target_size, inode_.blob_size);
        return ZX_ERR_IO_DATA_INTEGRITY;
    }

    blobfs_->UpdateMerkleDecompressMetrics((compressed_blocks) * kBlobfsBlockSize,
                                           inode_.blob_size, read_time, ticker.End());
    return ZX_OK;
}

zx_status_t VnodeBlob::InitUncompressed() {
    TRACE_DURATION("blobfs", "Blobfs::InitUncompressed", "size", inode_.blob_size,
                   "blocks", inode_.block_count);
    fs::Ticker ticker(blobfs_->CollectingMetrics());
    fs::ReadTxn txn(blobfs_);
    AllocatedExtentIterator extent_iter(blobfs_->allocator_.get(), GetMapIndex());
    BlockIterator block_iter(&extent_iter);
    // Read both the uncompressed merkle tree and data.
    const uint64_t blob_data_blocks = BlobDataBlocks(inode_);
    const uint64_t merkle_blocks = MerkleTreeBlocks(inode_);
    if (blob_data_blocks + merkle_blocks  > std::numeric_limits<uint32_t>::max()) {
        return ZX_ERR_IO_DATA_INTEGRITY;
    }
    const uint32_t length = static_cast<uint32_t>(blob_data_blocks + merkle_blocks);
    const uint64_t data_start = DataStartBlock(blobfs_->info_);
    zx_status_t status = StreamBlocks(&block_iter, length,
        [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
            txn.Enqueue(vmoid_, vmo_offset, dev_offset + data_start, length);
            return ZX_OK;
        });

    if (status != ZX_OK) {
        return status;
    }

    status = txn.Transact();
    if (status != ZX_OK) {
        return status;
    }
    blobfs_->UpdateMerkleDiskReadMetrics(length * kBlobfsBlockSize, ticker.End());
    return status;
}

void VnodeBlob::PopulateInode(uint32_t node_index) {
    ZX_DEBUG_ASSERT(map_index_ == 0);
    SetState(kBlobStateReadable);
    map_index_ = node_index;
    Inode* inode = blobfs_->GetNode(node_index);
    inode_ = *inode;
}

uint64_t VnodeBlob::SizeData() const {
    if (GetState() == kBlobStateReadable) {
        return inode_.blob_size;
    }
    return 0;
}

VnodeBlob::VnodeBlob(Blobfs* bs, const Digest& digest)
    : blobfs_(bs),
      flags_(kBlobStateEmpty), syncing_(false), clone_watcher_(this) {
    digest.CopyTo(digest_, sizeof(digest_));
}

VnodeBlob::VnodeBlob(Blobfs* bs)
    : blobfs_(bs),
      flags_(kBlobStateEmpty | kBlobFlagDirectory),
      syncing_(false), clone_watcher_(this) {}

void VnodeBlob::BlobCloseHandles() {
    mapping_.Reset();
    readable_event_.reset();
}

zx_status_t VnodeBlob::SpaceAllocate(uint64_t size_data) {
    TRACE_DURATION("blobfs", "Blobfs::SpaceAllocate", "size_data", size_data);
    fs::Ticker ticker(blobfs_->CollectingMetrics());

    if (GetState() != kBlobStateEmpty) {
        return ZX_ERR_BAD_STATE;
    }

    auto write_info = fbl::make_unique<WritebackInfo>();

    // Initialize the inode with known fields.
    memset(inode_.merkle_root_hash, 0, Digest::kLength);
    inode_.blob_size = size_data;
    inode_.block_count = MerkleTreeBlocks(inode_) + static_cast<uint32_t>(BlobDataBlocks(inode_));

    // Special case for the null blob: We skip the write phase.
    if (inode_.blob_size == 0) {
        zx_status_t status = blobfs_->ReserveNodes(1, &write_info->node_indices);
        if (status != ZX_OK) {
            return status;
        }
        map_index_ = write_info->node_indices[0].index();
        write_info_ = std::move(write_info);

        if ((status = Verify()) != ZX_OK) {
            return status;
        }
        SetState(kBlobStateDataWrite);
        if ((status = WriteMetadata()) != ZX_OK) {
            fprintf(stderr, "Null blob metadata fail: %d\n", status);
            return status;
        }
        return ZX_OK;
    }

    fbl::Vector<ReservedExtent> extents;
    fbl::Vector<ReservedNode> nodes;

    // Reserve space for the blob.
    zx_status_t status = blobfs_->ReserveBlocks(inode_.block_count, &extents);
    if (status != ZX_OK) {
        return status;
    }
    if (extents.size() > kMaxBlobExtents) {
        fprintf(stderr, "Error: Block reservation requires too many extents (%zu vs %zu max)\n",
                extents.size(), kMaxBlobExtents);
        return ZX_ERR_BAD_STATE;
    }
    const ExtentCountType extent_count = static_cast<ExtentCountType>(extents.size());

    // Reserve space for all the nodes necessary to contain this blob.
    size_t node_count = NodePopulator::NodeCountForExtents(extent_count);
    status = blobfs_->ReserveNodes(node_count, &nodes);
    if (status != ZX_OK) {
        return status;
    }

    if (inode_.blob_size >= kCompressionMinBytesSaved) {
        size_t max = write_info->compressor.BufferMax(inode_.blob_size);
        status = write_info->compressed_blob.CreateAndMap(max, "compressed-blob");
        if (status != ZX_OK) {
            return status;
        }
        status = write_info->compressor.Initialize(write_info->compressed_blob.start(),
                                                   write_info->compressed_blob.size());
        if (status != ZX_OK) {
            fprintf(stderr, "blobfs: Failed to initialize compressor: %d\n", status);
            return status;
        }
    }

    // Open VMOs, so we can begin writing after allocate succeeds.
    fzl::OwnedVmoMapper mapping;
    if ((status = mapping.CreateAndMap(inode_.block_count * kBlobfsBlockSize, "blob")) != ZX_OK) {
        return status;
    }
    if ((status = blobfs_->AttachVmo(mapping.vmo(), &vmoid_)) != ZX_OK) {
        return status;
    }

    map_index_ = nodes[0].index();
    mapping_ = std::move(mapping);
    write_info->extents = std::move(extents);
    write_info->node_indices = std::move(nodes);
    write_info_ = std::move(write_info);

    SetState(kBlobStateDataWrite);
    blobfs_->UpdateAllocationMetrics(size_data, ticker.End());
    return ZX_OK;
}

void* VnodeBlob::GetData() const {
    return fs::GetBlock(kBlobfsBlockSize, mapping_.start(), MerkleTreeBlocks(inode_));
}

void* VnodeBlob::GetMerkle() const {
    return mapping_.start();
}

zx_status_t VnodeBlob::WriteMetadata() {
    TRACE_DURATION("blobfs", "Blobfs::WriteMetadata");
    assert(GetState() == kBlobStateDataWrite);

    zx_status_t status;
    fbl::unique_ptr<WritebackWork> wb;
    if ((status = blobfs_->CreateWork(&wb, this)) != ZX_OK) {
        return status;
    }

    // Update the on-disk hash.
    memcpy(inode_.merkle_root_hash, &digest_[0], Digest::kLength);

    // All data has been written to the containing VMO.
    SetState(kBlobStateReadable);
    if (readable_event_.is_valid()) {
        status = readable_event_.signal(0u, ZX_USER_SIGNAL_0);
        if (status != ZX_OK) {
            SetState(kBlobStateError);
            return status;
        }
    }

    atomic_store(&syncing_, true);

    if (inode_.block_count) {
        // We utilize the NodePopulator class to take our reserved blocks and nodes and fill the
        // persistent map with an allocated inode / container.

        // If |on_node| is invoked on a node, it means that node was necessary to represent this
        // blob. Persist the node back to durable storge.
        auto on_node = [this, &wb](const ReservedNode& node) {
            blobfs_->PersistNode(wb.get(), node.index());
        };

        // If |on_extent| is invoked on an extent, it was necessary to represent this blob. Persist
        // the allocation of these blocks back to durable storage.
        //
        // Additionally, because of the compression feature of blobfs, it is possible we reserved
        // more extents than this blob ended up using. Decrement |remaining_blocks| to track if we
        // should exit early.
        size_t remaining_blocks = inode_.block_count;
        auto on_extent = [this, &wb, &remaining_blocks](ReservedExtent& extent) {
            ZX_DEBUG_ASSERT(remaining_blocks > 0);
            if (remaining_blocks >= extent.extent().Length()) {
                // Consume the entire extent.
                remaining_blocks -= extent.extent().Length();
            } else {
                // Consume only part of the extent; we're done iterating.
                extent.SplitAt(static_cast<BlockCountType>(remaining_blocks));
                remaining_blocks = 0;
            }
            blobfs_->PersistBlocks(wb.get(), extent);
            if (remaining_blocks == 0) {
                return NodePopulator::IterationCommand::Stop;
            }
            return NodePopulator::IterationCommand::Continue;
        };

        Inode* mapped_inode = blobfs_->GetNode(map_index_);
        *mapped_inode = inode_;
        NodePopulator populator(blobfs_->allocator_.get(), std::move(write_info_->extents),
                                std::move(write_info_->node_indices));
        ZX_ASSERT(populator.Walk(on_node, on_extent) == ZX_OK);

        // Ensure all non-allocation flags are propagated to the inode.
        mapped_inode->header.flags |= (inode_.header.flags & kBlobFlagLZ4Compressed);
    } else {
        // Special case: Empty node.
        ZX_DEBUG_ASSERT(write_info_->node_indices.size() == 1);
        const ReservedNode& node = write_info_->node_indices[0];
        blobfs_->allocator_->MarkInodeAllocated(node);
        blobfs_->PersistNode(wb.get(), node.index());
    }

    wb->SetSyncComplete();
    if ((status = blobfs_->EnqueueWork(std::move(wb), EnqueueType::kJournal)) != ZX_OK) {
        return status;
    }

    // Drop the write info, since we no longer need it.
    write_info_.reset();
    return status;
}

zx_status_t VnodeBlob::WriteInternal(const void* data, size_t len, size_t* actual) {
    TRACE_DURATION("blobfs", "Blobfs::WriteInternal", "data", data, "len", len);

    *actual = 0;
    if (len == 0) {
        return ZX_OK;
    }

    const uint32_t merkle_blocks = MerkleTreeBlocks(inode_);
    const size_t merkle_bytes = merkle_blocks * kBlobfsBlockSize;
    if (GetState() == kBlobStateDataWrite) {
        size_t to_write = fbl::min(len, inode_.blob_size - write_info_->bytes_written);
        size_t offset = write_info_->bytes_written + merkle_bytes;
        zx_status_t status = mapping_.vmo().write(data, offset, to_write);
        if (status != ZX_OK) {
            return status;
        }

        *actual = to_write;
        write_info_->bytes_written += to_write;

        if (write_info_->compressor.Compressing()) {
            if ((status = write_info_->compressor.Update(data, to_write)) != ZX_OK) {
                return status;
            }
            ConsiderCompressionAbort();
        }

        // More data to write.
        if (write_info_->bytes_written < inode_.blob_size) {
            return ZX_OK;
        }

        // Only write data to disk once we've buffered the file into memory.
        // This gives us a chance to try compressing the blob before we write it back.
        fbl::unique_ptr<WritebackWork> wb;
        if ((status = blobfs_->CreateWork(&wb, this)) != ZX_OK) {
            return status;
        }

        // In case the operation fails, forcibly reset the WritebackWork
        // to avoid asserting that no write requests exist on destruction.
        auto set_error = fbl::MakeAutoCall([&]() {
            if (wb != nullptr) {
                wb->Reset(ZX_ERR_BAD_STATE);
            }

            SetState(kBlobStateError);
        });

        if (write_info_->compressor.Compressing()) {
            if ((status = write_info_->compressor.End()) != ZX_OK) {
                return status;
            }
            ConsiderCompressionAbort();
        }

        // Since the merkle tree and data are co-allocated, use a block iterator
        // to parse their data in order.
        VectorExtentIterator extent_iter(write_info_->extents);
        BlockIterator block_iter(&extent_iter);

        // TODO(smklein): As an optimization, use the CreateInit/Update/Final
        // methods to create the merkle tree as we write data, rather than
        // waiting until the data is fully downloaded to create the tree.
        size_t merkle_size = MerkleTree::GetTreeLength(inode_.blob_size);
        fs::Duration generation_time;
        if (merkle_size > 0) {
            Digest digest;
            void* merkle_data = GetMerkle();
            const void* blob_data = GetData();
            fs::Ticker ticker(blobfs_->CollectingMetrics()); // Tracking generation time.

            if ((status = MerkleTree::Create(blob_data, inode_.blob_size, merkle_data,
                                             merkle_size, &digest)) != ZX_OK) {
                return status;
            } else if (digest != digest_) {
                // Downloaded blob did not match provided digest.
                return ZX_ERR_IO_DATA_INTEGRITY;
            }

            status = StreamBlocks(&block_iter, merkle_blocks,
                [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                    return EnqueuePaginated(&wb, blobfs_, this, mapping_.vmo(), vmo_offset,
                                            dev_offset + blobfs_->DataStart(), length);
                });

            if (status != ZX_OK) {
                return status;
            }
            generation_time = ticker.End();
        } else if ((status = Verify()) != ZX_OK) {
            // Small blobs may not have associated Merkle Trees, and will
            // require validation, since we are not regenerating and checking
            // the digest.
            return status;
        }

        if (write_info_->compressor.Compressing()) {
            uint64_t blocks64 = fbl::round_up(write_info_->compressor.Size(),
                                              kBlobfsBlockSize) / kBlobfsBlockSize;
            ZX_DEBUG_ASSERT(blocks64 <= std::numeric_limits<uint32_t>::max());
            uint32_t blocks = static_cast<uint32_t>(blocks64);
            int64_t vmo_bias = -static_cast<int64_t>(merkle_blocks);
            ZX_DEBUG_ASSERT(block_iter.BlockIndex() + vmo_bias == 0);
            status = StreamBlocks(&block_iter, blocks,
                [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                    return EnqueuePaginated(&wb, blobfs_, this, write_info_->compressed_blob.vmo(),
                                            vmo_offset - merkle_blocks,
                                            dev_offset + blobfs_->DataStart(), length);
                });

            if (status != ZX_OK) {
                return status;
            }
            blocks += MerkleTreeBlocks(inode_);
            // By compressing, we used less blocks than we originally reserved.
            ZX_DEBUG_ASSERT(inode_.block_count > blocks);

            inode_.block_count = blocks;
            inode_.header.flags |= kBlobFlagLZ4Compressed;
        } else {
            uint64_t blocks64 =
                    fbl::round_up(inode_.blob_size, kBlobfsBlockSize) / kBlobfsBlockSize;
            ZX_DEBUG_ASSERT(blocks64 <= std::numeric_limits<uint32_t>::max());
            uint32_t blocks = static_cast<uint32_t>(blocks64);
            status = StreamBlocks(&block_iter, blocks,
                [&](uint64_t vmo_offset, uint64_t dev_offset, uint32_t length) {
                    return EnqueuePaginated(&wb, blobfs_, this, mapping_.vmo(), vmo_offset,
                                            dev_offset + blobfs_->DataStart(), length);
                });
            if (status != ZX_OK) {
                return status;
            }
        }

        // Enqueue the blob's final data work. Metadata must be enqueued separately.
        if ((status = blobfs_->EnqueueWork(std::move(wb), EnqueueType::kData)) != ZX_OK) {
            return status;
        }

        // No more data to write. Flush to disk.
        fs::Ticker ticker(blobfs_->CollectingMetrics()); // Tracking enqueue time.
        if ((status = WriteMetadata()) != ZX_OK) {
            return status;
        }

        blobfs_->UpdateClientWriteMetrics(to_write, merkle_size, ticker.End(),
                                          generation_time);
        set_error.cancel();
        return ZX_OK;
    }

    return ZX_ERR_BAD_STATE;
}

void VnodeBlob::ConsiderCompressionAbort() {
    ZX_DEBUG_ASSERT(write_info_->compressor.Compressing());
    if (inode_.blob_size - kCompressionMinBytesSaved < write_info_->compressor.Size()) {
        write_info_->compressor.Reset();
        write_info_->compressed_blob.Reset();
    }
}

zx_status_t VnodeBlob::GetReadableEvent(zx_handle_t* out) {
    TRACE_DURATION("blobfs", "Blobfs::GetReadableEvent");
    zx_status_t status;
    // This is the first 'wait until read event' request received.
    if (!readable_event_.is_valid()) {
        status = zx::event::create(0, &readable_event_);
        if (status != ZX_OK) {
            return status;
        } else if (GetState() == kBlobStateReadable) {
            readable_event_.signal(0u, ZX_USER_SIGNAL_0);
        }
    }
    status = zx_handle_duplicate(readable_event_.get(), ZX_RIGHTS_BASIC, out);
    if (status != ZX_OK) {
        return status;
    }
    return sizeof(zx_handle_t);
}

zx_status_t VnodeBlob::CloneVmo(zx_rights_t rights, zx_handle_t* out) {
    TRACE_DURATION("blobfs", "Blobfs::CloneVmo", "rights", rights, "out", out);
    if (GetState() != kBlobStateReadable) {
        return ZX_ERR_BAD_STATE;
    }
    if (inode_.blob_size == 0) {
        return ZX_ERR_BAD_STATE;
    }
    zx_status_t status = InitVmos();
    if (status != ZX_OK) {
        return status;
    }

    // TODO(smklein): Only clone / verify the part of the vmo that
    // was requested.
    const size_t merkle_bytes = MerkleTreeBlocks(inode_) * kBlobfsBlockSize;
    zx::vmo clone;
    if ((status = mapping_.vmo().clone(ZX_VMO_CLONE_COPY_ON_WRITE, merkle_bytes, inode_.blob_size,
                                       &clone)) != ZX_OK) {
        return status;
    }

    // TODO(mdempsky): Push elsewhere.
    if ((status = clone.replace_as_executable(zx::handle(), &clone)) != ZX_OK) {
        return status;
    }

    if ((status = clone.replace(rights, &clone)) != ZX_OK) {
        return status;
    }
    *out = clone.release();

    if (clone_watcher_.object() == ZX_HANDLE_INVALID) {
        clone_watcher_.set_object(mapping_.vmo().get());
        clone_watcher_.set_trigger(ZX_VMO_ZERO_CHILDREN);

        // Keep a reference to "this" alive, preventing the blob
        // from being closed while someone may still be using the
        // underlying memory.
        //
        // We'll release it when no client-held VMOs are in use.
        clone_ref_ = fbl::RefPtr<VnodeBlob>(this);
        clone_watcher_.Begin(blobfs_->dispatcher());
    }

    return ZX_OK;
}

void VnodeBlob::HandleNoClones(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                               zx_status_t status, const zx_packet_signal_t* signal) {
    ZX_DEBUG_ASSERT(status == ZX_OK);
    ZX_DEBUG_ASSERT((signal->observed & ZX_VMO_ZERO_CHILDREN) != 0);
    ZX_DEBUG_ASSERT(clone_watcher_.object() != ZX_HANDLE_INVALID);
    clone_watcher_.set_object(ZX_HANDLE_INVALID);
    clone_ref_ = nullptr;
}

zx_status_t VnodeBlob::ReadInternal(void* data, size_t len, size_t off, size_t* actual) {
    TRACE_DURATION("blobfs", "Blobfs::ReadInternal", "len", len, "off", off);

    if (GetState() != kBlobStateReadable) {
        return ZX_ERR_BAD_STATE;
    }

    if (inode_.blob_size == 0) {
        *actual = 0;
        return ZX_OK;
    }

    zx_status_t status = InitVmos();
    if (status != ZX_OK) {
        return status;
    }

    Digest d;
    d = reinterpret_cast<const uint8_t*>(&digest_[0]);

    if (off >= inode_.blob_size) {
        *actual = 0;
        return ZX_OK;
    }
    if (len > (inode_.blob_size - off)) {
        len = inode_.blob_size - off;
    }

    const size_t merkle_bytes = MerkleTreeBlocks(inode_) * kBlobfsBlockSize;
    status = mapping_.vmo().read(data, merkle_bytes + off, len);
    if (status == ZX_OK) {
        *actual = len;
    }
    return status;
}

zx_status_t VnodeBlob::QueueUnlink() {
    flags_ |= kBlobFlagDeletable;
    // Attempt to purge in case the blob has been unlinked with no open fds
    return TryPurge();
}

zx_status_t VnodeBlob::VerifyBlob(Blobfs* bs, uint32_t node_index) {
    Inode* inode = bs->GetNode(node_index);
    Digest digest(inode->merkle_root_hash);
    fbl::AllocChecker ac;
    fbl::RefPtr<VnodeBlob> vn =
        fbl::AdoptRef(new (&ac) VnodeBlob(bs, digest));

    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    vn->PopulateInode(node_index);

    // Set blob state to "Purged" so we do not try to add it to the cached map on recycle.
    vn->SetState(kBlobStatePurged);

    // If we are unable to read in the blob from disk, this should also be a VerifyBlob error.
    // Since InitVmos calls Verify as its final step, we can just return its result here.
    return vn->InitVmos();
}

zx_status_t Blobfs::VerifyBlob(uint32_t node_index) {
    return VnodeBlob::VerifyBlob(this, node_index);
}

void Blobfs::PersistBlocks(WritebackWork* wb, const ReservedExtent& reserved_extent) {
    TRACE_DURATION("blobfs", "Blobfs::PersistBlocks");

    allocator_->MarkBlocksAllocated(reserved_extent);

    const Extent& extent = reserved_extent.extent();
    info_.alloc_block_count += extent.Length();
    // Write out to disk.
    WriteBitmap(wb, extent.Length(), extent.Start());
    WriteInfo(wb);
}

// Frees blocks from reserved and allocated maps, updates disk in the latter case.
void Blobfs::FreeExtent(WritebackWork* wb, const Extent& extent) {
    size_t start = extent.Start();
    size_t num_blocks = extent.Length();
    size_t end = start + num_blocks;

    TRACE_DURATION("blobfs", "Blobfs::FreeExtent", "nblocks", num_blocks, "blkno", start);

    // Check if blocks were allocated on disk.
    if (allocator_->CheckBlocksAllocated(start, end)) {
        allocator_->FreeBlocks(extent);
        info_.alloc_block_count -= num_blocks;
        WriteBitmap(wb, num_blocks, start);
        WriteInfo(wb);
    }
}

void Blobfs::FreeNode(WritebackWork* wb, uint32_t node_index) {
    allocator_->FreeNode(node_index);
    info_.alloc_inode_count--;
    WriteNode(wb, node_index);
}

void Blobfs::FreeInode(WritebackWork* wb, uint32_t node_index) {
    TRACE_DURATION("blobfs", "Blobfs::FreeInode", "node_index", node_index);
    Inode* mapped_inode = GetNode(node_index);
    ZX_DEBUG_ASSERT(wb != nullptr);

    if (mapped_inode->header.IsAllocated()) {
        // Always write back the first node.
        FreeNode(wb, node_index);

        AllocatedExtentIterator extent_iter(allocator_.get(), node_index);
        while (!extent_iter.Done()) {
            // If we're observing a new node, free it.
            if (extent_iter.NodeIndex() != node_index) {
                node_index = extent_iter.NodeIndex();
                FreeNode(wb, node_index);
            }

            const Extent* extent;
            ZX_ASSERT(extent_iter.Next(&extent) == ZX_OK);

            // Free the extent.
            FreeExtent(wb, *extent);
        }
        WriteInfo(wb);
    }
}

void Blobfs::PersistNode(WritebackWork* wb, uint32_t node_index) {
    TRACE_DURATION("blobfs", "Blobfs::PersistNode");
    info_.alloc_inode_count++;
    WriteNode(wb, node_index);
    WriteInfo(wb);
}

zx_status_t Blobfs::InitializeWriteback(const MountOptions& options) {
    if (options.readonly) {
        // If blobfs should be readonly, do not start up any writeback threads.
        return ZX_OK;
    }

    // Initialize the WritebackQueue.
    zx_status_t status = WritebackQueue::Create(this, WriteBufferSize() / kBlobfsBlockSize,
                                                &writeback_);

    if (status != ZX_OK) {
        return status;
    }

    // Replay any lingering journal entries.
    if ((status = journal_->Replay()) != ZX_OK) {
        return status;
    }

    // TODO(ZX-2728): Don't load metadata until after journal replay.
    // Re-load blobfs metadata from disk, since things may have changed.
    if ((status = Reload()) != ZX_OK) {
        return status;
    }

    if (options.journal) {
        // Initialize the journal's writeback thread (if journaling is enabled).
        // Wait until after replay has completed in order to avoid concurrency issues.
        return journal_->InitWriteback();
    }

    // If journaling is disabled, delete the journal.
    journal_.reset();
    return ZX_OK;
}

size_t Blobfs::WritebackCapacity() const { return writeback_->GetCapacity(); }

void Blobfs::Shutdown(fs::Vfs::ShutdownCallback cb) {
    TRACE_DURATION("blobfs", "Blobfs::Unmount");

    // 1) Shutdown all external connections to blobfs.
    ManagedVfs::Shutdown([this, cb = std::move(cb)](zx_status_t status) mutable {
        // 2a) Shutdown all internal connections to blobfs.
        // Store the Vnodes in a vector to avoid destroying
        // them while holding the hash lock.
        fbl::Vector<fbl::RefPtr<VnodeBlob>> internal_references;
        {
            fbl::AutoLock lock(&hash_lock_);
            for (auto& blob : open_hash_) {
                auto vn = blob.CloneWatcherTeardown();
                if (vn != nullptr) {
                    internal_references.push_back(std::move(vn));
                }
            }
        }
        internal_references.reset();

        // 2b) Flush all pending work to blobfs to the underlying storage.
        Sync([this, cb = std::move(cb)](zx_status_t status) mutable {
            async::PostTask(dispatcher(), [this, cb = std::move(cb)]() mutable {
                // 3) Ensure the underlying disk has also flushed.
                {
                    fs::WriteTxn sync_txn(this);
                    sync_txn.EnqueueFlush();
                    sync_txn.Transact();
                    // Although the transaction shouldn't reference 'this'
                    // after completing, scope it here to be extra cautious.
                }

                DumpMetrics();

                auto on_unmount = std::move(on_unmount_);

                // Manually destroy Blobfs. The promise of Shutdown is that no
                // connections are active, and destroying the Blobfs object
                // should terminate all background workers.
                delete this;

                // Identify to the unmounting channel that we've completed teardown.
                cb(ZX_OK);

                // Identify to the mounting thread that the filesystem has
                // terminated.
                if (on_unmount) {
                    on_unmount();
                }
            });
        });
    });
}

void Blobfs::WriteBitmap(WritebackWork* wb, uint64_t nblocks, uint64_t start_block) {
    TRACE_DURATION("blobfs", "Blobfs::WriteBitmap", "nblocks", nblocks, "start_block",
                   start_block);
    uint64_t bbm_start_block = start_block / kBlobfsBlockBits;
    uint64_t bbm_end_block = fbl::round_up(start_block + nblocks,
                                           kBlobfsBlockBits) /
                             kBlobfsBlockBits;

    // Write back the block allocation bitmap
    wb->Enqueue(allocator_->GetBlockMapVmo(), bbm_start_block,
                BlockMapStartBlock(info_) + bbm_start_block, bbm_end_block - bbm_start_block);
}

void Blobfs::WriteNode(WritebackWork* wb, uint32_t map_index) {
    TRACE_DURATION("blobfs", "Blobfs::WriteNode", "map_index", map_index);
    uint64_t b = (map_index * sizeof(Inode)) / kBlobfsBlockSize;
    wb->Enqueue(allocator_->GetNodeMapVmo(), b, NodeMapStartBlock(info_) + b, 1);
}

zx_status_t Blobfs::NewBlob(const Digest& digest, fbl::RefPtr<VnodeBlob>* out) {
    TRACE_DURATION("blobfs", "Blobfs::NewBlob");
    zx_status_t status;
    // If the blob already exists (or we're having trouble looking up the blob),
    // return an error.
    if ((status = LookupBlob(digest, nullptr)) != ZX_ERR_NOT_FOUND) {
        return (status == ZX_OK) ? ZX_ERR_ALREADY_EXISTS : status;
    }

    fbl::AllocChecker ac;
    *out = fbl::AdoptRef(new (&ac) VnodeBlob(this, digest));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    fbl::AutoLock lock(&hash_lock_);
    open_hash_.insert(out->get());
    return ZX_OK;
}

// If no client references to the blob still exist and the blob is either queued for deletion or
// not in a readable state, purge all traces of the blob from blobfs.
// This is only called when we do not expect the blob to be accessed again.
zx_status_t Blobfs::PurgeBlob(VnodeBlob* vn) {
    TRACE_DURATION("blobfs", "Blobfs::PurgeBlob");

    switch (vn->GetState()) {
    case kBlobStateEmpty:
    case kBlobStateDataWrite:
    case kBlobStateError: {
        VnodeReleaseHard(vn);
        return ZX_OK;
    }
    case kBlobStateReadable: {
        // A readable blob should only be purged if it has been unlinked.
        ZX_ASSERT(vn->DeletionQueued());
        uint32_t node_index = vn->GetMapIndex();
        zx_status_t status;
        fbl::unique_ptr<WritebackWork> wb;
        if ((status = CreateWork(&wb, vn)) != ZX_OK) {
            return status;
        }

        FreeInode(wb.get(), node_index);
        VnodeReleaseHard(vn);
        return EnqueueWork(std::move(wb), EnqueueType::kJournal);
    }
    default: {
        assert(false);
    }
    }
    return ZX_ERR_NOT_SUPPORTED;
}

void Blobfs::WriteInfo(WritebackWork* wb) {
    memcpy(info_mapping_.start(), &info_, sizeof(info_));
    wb->Enqueue(info_mapping_.vmo(), 0, 0, 1);
}

zx_status_t Blobfs::CreateFsId() {
    ZX_DEBUG_ASSERT(!fs_id_);
    zx::event event;
    zx_status_t status = zx::event::create(0, &event);
    if (status != ZX_OK) {
        return status;
    }
    zx_info_handle_basic_t info;
    status = event.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK) {
        return status;
    }

    fs_id_ = info.koid;
    return ZX_OK;
}

typedef struct dircookie {
    size_t index;      // Index into node map
    uint64_t reserved; // Unused
} dircookie_t;

static_assert(sizeof(dircookie_t) <= sizeof(fs::vdircookie_t),
              "Blobfs dircookie too large to fit in IO state");

zx_status_t Blobfs::Readdir(fs::vdircookie_t* cookie, void* dirents, size_t len,
                            size_t* out_actual) {
    TRACE_DURATION("blobfs", "Blobfs::Readdir", "len", len);
    fs::DirentFiller df(dirents, len);
    dircookie_t* c = reinterpret_cast<dircookie_t*>(cookie);

    for (size_t i = c->index; i < info_.inode_count; ++i) {
        ZX_DEBUG_ASSERT(i < std::numeric_limits<uint32_t>::max());
        uint32_t node_index = static_cast<uint32_t>(i);
        if (GetNode(node_index)->header.IsAllocated() &&
            !GetNode(node_index)->header.IsExtentContainer()) {
            Digest digest(GetNode(node_index)->merkle_root_hash);
            char name[Digest::kLength * 2 + 1];
            zx_status_t r = digest.ToString(name, sizeof(name));
            if (r < 0) {
                return r;
            }
            uint64_t ino = fuchsia_io_INO_UNKNOWN;
            if ((r = df.Next(fbl::StringPiece(name, Digest::kLength * 2),
                             VTYPE_TO_DTYPE(V_TYPE_FILE), ino)) != ZX_OK) {
                break;
            }
            c->index = i + 1;
        }
    }

    *out_actual = df.BytesFilled();
    return ZX_OK;
}

zx_status_t Blobfs::LookupBlob(const Digest& digest, fbl::RefPtr<VnodeBlob>* out) {
    TRACE_DURATION("blobfs", "Blobfs::LookupBlob");
    const uint8_t* key = digest.AcquireBytes();
    auto release = fbl::MakeAutoCall([&digest]() {
        digest.ReleaseBytes();
    });

    // Look up the blob in the maps.
    fbl::RefPtr<VnodeBlob> vn;
    while (true) {
        // Avoid releasing a reference to |vn| while holding |hash_lock_|.
        fbl::AutoLock lock(&hash_lock_);
        auto raw_vn = open_hash_.find(key).CopyPointer();
        if (raw_vn != nullptr) {
            vn = fbl::MakeRefPtrUpgradeFromRaw(raw_vn, hash_lock_);
            if (vn == nullptr) {
                // This condition is only possible if:
                // - The raw pointer to the Vnode exists in the open map,
                // with refcount == 0.
                // - Another thread is fbl_recycling this Vnode, but has not
                // yet resurrected it.
                // - The vnode is being moved to the close cache, and is
                // not yet purged.
                //
                // It is not safe for us to attempt to Resurrect the Vnode. If
                // we do so, then the caller of LookupBlob may unlink, purge, and
                // destroy the Vnode concurrently before the original caller of
                // "fbl_recycle" completes.
                //
                // Since the window of time for this condition is extremely
                // small (between Release and the resurrection of the Vnode),
                // and only contains a single flag check, we unlock and try
                // again.
                continue;
            }
        } else {
            vn = VnodeUpgradeLocked(key);
        }
        break;
    }

    if (vn != nullptr) {
        UpdateLookupMetrics(vn->SizeData());
        if (out != nullptr) {
            *out = std::move(vn);
        }
        return ZX_OK;
    }

    return ZX_ERR_NOT_FOUND;
}

zx_status_t Blobfs::AttachVmo(const zx::vmo& vmo, vmoid_t* out) {
    zx::vmo xfer_vmo;
    zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo);
    if (status != ZX_OK) {
        return status;
    }
    zx_handle_t raw_vmo = xfer_vmo.release();
    ssize_t r = ioctl_block_attach_vmo(Fd(), &raw_vmo, out);
    if (r < 0) {
        return static_cast<zx_status_t>(r);
    }
    return ZX_OK;
}

zx_status_t Blobfs::DetachVmo(vmoid_t vmoid) {
    block_fifo_request_t request;
    request.group = BlockGroupID();
    request.vmoid = vmoid;
    request.opcode = BLOCKIO_CLOSE_VMO;
    return Transaction(&request, 1);
}

zx_status_t Blobfs::AddInodes(fzl::ResizeableVmoMapper* node_map) {
    TRACE_DURATION("blobfs", "Blobfs::AddInodes");

    if (!(info_.flags & kBlobFlagFVM)) {
        return ZX_ERR_NO_SPACE;
    }

    const size_t kBlocksPerSlice = info_.slice_size / kBlobfsBlockSize;
    extend_request_t request;
    request.length = 1;
    request.offset = (kFVMNodeMapStart / kBlocksPerSlice) + info_.ino_slices;
    if (ioctl_block_fvm_extend(Fd(), &request) < 0) {
        fprintf(stderr, "Blobfs::AddInodes fvm_extend failure");
        return ZX_ERR_NO_SPACE;
    }

    const uint32_t kInodesPerSlice = static_cast<uint32_t>(info_.slice_size / kBlobfsInodeSize);
    uint64_t inodes64 = (info_.ino_slices + static_cast<uint32_t>(request.length)) * kInodesPerSlice;
    ZX_DEBUG_ASSERT(inodes64 <= std::numeric_limits<uint32_t>::max());
    uint32_t inodes = static_cast<uint32_t>(inodes64);
    uint32_t inoblks = (inodes + kBlobfsInodesPerBlock - 1) / kBlobfsInodesPerBlock;
    ZX_DEBUG_ASSERT(info_.inode_count <= std::numeric_limits<uint32_t>::max());
    uint32_t inoblks_old = (static_cast<uint32_t>(info_.inode_count) + kBlobfsInodesPerBlock - 1) / kBlobfsInodesPerBlock;
    ZX_DEBUG_ASSERT(inoblks_old <= inoblks);

    if (node_map->Grow(inoblks * kBlobfsBlockSize) != ZX_OK) {
        return ZX_ERR_NO_SPACE;
    }

    info_.vslice_count += request.length;
    info_.ino_slices += static_cast<uint32_t>(request.length);
    info_.inode_count = inodes;

    // Reset new inodes to 0
    uintptr_t addr = reinterpret_cast<uintptr_t>(node_map->start());
    memset(reinterpret_cast<void*>(addr + kBlobfsBlockSize * inoblks_old), 0,
           (kBlobfsBlockSize * (inoblks - inoblks_old)));

    zx_status_t status;
    fbl::unique_ptr<WritebackWork> wb;
    if ((status = CreateWork(&wb, nullptr)) != ZX_OK) {
        return status;
    }

    WriteInfo(wb.get());
    wb.get()->Enqueue(node_map->vmo(), inoblks_old, NodeMapStartBlock(info_) + inoblks_old,
                      inoblks - inoblks_old);
    return EnqueueWork(std::move(wb), EnqueueType::kJournal);
}

zx_status_t Blobfs::AddBlocks(size_t nblocks, RawBitmap* block_map) {
    TRACE_DURATION("blobfs", "Blobfs::AddBlocks", "nblocks", nblocks);

    if (!(info_.flags & kBlobFlagFVM)) {
        return ZX_ERR_NO_SPACE;
    }

    const size_t kBlocksPerSlice = info_.slice_size / kBlobfsBlockSize;
    extend_request_t request;
    // Number of slices required to add nblocks
    request.length = (nblocks + kBlocksPerSlice - 1) / kBlocksPerSlice;
    request.offset = (kFVMDataStart / kBlocksPerSlice) + info_.dat_slices;

    uint64_t blocks64 = (info_.dat_slices + request.length) * kBlocksPerSlice;
    ZX_DEBUG_ASSERT(blocks64 <= std::numeric_limits<uint32_t>::max());
    uint32_t blocks = static_cast<uint32_t>(blocks64);
    uint32_t abmblks = (blocks + kBlobfsBlockBits - 1) / kBlobfsBlockBits;
    uint64_t abmblks_old = (info_.data_block_count + kBlobfsBlockBits - 1) / kBlobfsBlockBits;
    ZX_DEBUG_ASSERT(abmblks_old <= abmblks);

    if (abmblks > kBlocksPerSlice) {
        //TODO(planders): Allocate more slices for the block bitmap.
        fprintf(stderr, "Blobfs::AddBlocks needs to increase block bitmap size\n");
        return ZX_ERR_NO_SPACE;
    }

    if (ioctl_block_fvm_extend(Fd(), &request) < 0) {
        fprintf(stderr, "Blobfs::AddBlocks FVM Extend failure\n");
        return ZX_ERR_NO_SPACE;
    }

    // Grow the block bitmap to hold new number of blocks
    if (block_map->Grow(fbl::round_up(blocks, kBlobfsBlockBits)) != ZX_OK) {
        return ZX_ERR_NO_SPACE;
    }
    // Grow before shrinking to ensure the underlying storage is a multiple
    // of kBlobfsBlockSize.
    block_map->Shrink(blocks);

    zx_status_t status;
    fbl::unique_ptr<WritebackWork> wb;
    if ((status = CreateWork(&wb, nullptr)) != ZX_OK) {
        return status;
    }

    // Since we are extending the bitmap, we need to fill the expanded
    // portion of the allocation block bitmap with zeroes.
    if (abmblks > abmblks_old) {
        uint64_t vmo_offset = abmblks_old;
        uint64_t dev_offset = BlockMapStartBlock(info_) + abmblks_old;
        uint64_t length = abmblks - abmblks_old;
        wb.get()->Enqueue(block_map->StorageUnsafe()->GetVmo(), vmo_offset, dev_offset, length);
    }

    info_.vslice_count += request.length;
    info_.dat_slices += static_cast<uint32_t>(request.length);
    info_.data_block_count = blocks;

    WriteInfo(wb.get());
    return EnqueueWork(std::move(wb), EnqueueType::kJournal);
}

void Blobfs::Sync(SyncCallback closure) {
    zx_status_t status;
    fbl::unique_ptr<WritebackWork> wb;
    if ((status = CreateWork(&wb, nullptr)) != ZX_OK) {
        closure(status);
        return;
    }

    wb->SetSyncCallback(std::move(closure));
    // This may return an error, but it doesn't matter - the closure will be called anyway.
    status = EnqueueWork(std::move(wb), EnqueueType::kJournal);
}

void Blobfs::UpdateAllocationMetrics(uint64_t size_data, const fs::Duration& duration) {
    if (CollectingMetrics()) {
        metrics_.blobs_created++;
        metrics_.blobs_created_total_size += size_data;
        metrics_.total_allocation_time_ticks += duration;
    }
}

void Blobfs::UpdateLookupMetrics(uint64_t size) {
    if (CollectingMetrics()) {
        metrics_.blobs_opened++;
        metrics_.blobs_opened_total_size += size;
    }
}

void Blobfs::UpdateClientWriteMetrics(uint64_t data_size, uint64_t merkle_size,
                                      const fs::Duration& enqueue_duration,
                                      const fs::Duration& generate_duration) {
    if (CollectingMetrics()) {
        metrics_.data_bytes_written += data_size;
        metrics_.merkle_bytes_written += merkle_size;
        metrics_.total_write_enqueue_time_ticks += enqueue_duration;
        metrics_.total_merkle_generation_time_ticks += generate_duration;
    }
}

void Blobfs::UpdateWritebackMetrics(uint64_t size, const fs::Duration& duration) {
    if (CollectingMetrics()) {
        metrics_.total_writeback_time_ticks += duration;
        metrics_.total_writeback_bytes_written += size;
    }
}

void Blobfs::UpdateMerkleDiskReadMetrics(uint64_t size, const fs::Duration& duration) {
    if (CollectingMetrics()) {
        metrics_.total_read_from_disk_time_ticks += duration;
        metrics_.bytes_read_from_disk += size;
    }
}

void Blobfs::UpdateMerkleDecompressMetrics(uint64_t size_compressed,
                                           uint64_t size_uncompressed,
                                           const fs::Duration& read_duration,
                                           const fs::Duration& decompress_duration) {
    if (CollectingMetrics()) {
        metrics_.bytes_compressed_read_from_disk += size_compressed;
        metrics_.bytes_decompressed_from_disk += size_uncompressed;
        metrics_.total_read_compressed_time_ticks += read_duration;
        metrics_.total_decompress_time_ticks += decompress_duration;
    }
}

void Blobfs::UpdateMerkleVerifyMetrics(uint64_t size_data, uint64_t size_merkle,
                                       const fs::Duration& duration) {
    if (CollectingMetrics()) {
        metrics_.blobs_verified++;
        metrics_.blobs_verified_total_size_data += size_data;
        metrics_.blobs_verified_total_size_merkle += size_merkle;
        metrics_.total_verification_time_ticks += duration;
    }
}

Blobfs::Blobfs(fbl::unique_fd fd, const Superblock* info)
    : blockfd_(std::move(fd)) {
    memcpy(&info_, info, sizeof(Superblock));
}

Blobfs::~Blobfs() {
    // The journal must be destroyed before the writeback buffer, since it may still need
    // to enqueue more transactions for writeback.
    journal_.reset();
    writeback_.reset();

    ZX_ASSERT(open_hash_.is_empty());
    closed_hash_.clear();

    if (blockfd_) {
        ioctl_block_fifo_close(Fd());
    }
}

zx_status_t Blobfs::Create(fbl::unique_fd fd, const MountOptions& options,
                           const Superblock* info, fbl::unique_ptr<Blobfs>* out) {
    TRACE_DURATION("blobfs", "Blobfs::Create");
    zx_status_t status = CheckSuperblock(info, TotalBlocks(*info));
    if (status < 0) {
        fprintf(stderr, "blobfs: Check info failure\n");
        return status;
    }

    fbl::AllocChecker ac;
    auto fs = fbl::unique_ptr<Blobfs>(new Blobfs(std::move(fd), info));
    fs->SetReadonly(options.readonly);
    fs->SetCachePolicy(options.cache_policy);
    if (options.metrics) {
        fs->CollectMetrics();
    }

    zx::fifo fifo;
    ssize_t r;
    if ((r = ioctl_block_get_info(fs->Fd(), &fs->block_info_)) < 0) {
        return static_cast<zx_status_t>(r);
    } else if (kBlobfsBlockSize % fs->block_info_.block_size != 0) {
        return ZX_ERR_IO;
    } else if ((r = ioctl_block_get_fifos(fs->Fd(), fifo.reset_and_get_address())) < 0) {
        fprintf(stderr, "Failed to mount blobfs: Someone else is using the block device\n");
        return static_cast<zx_status_t>(r);
    }

    if ((status = block_client::Client::Create(std::move(fifo), &fs->fifo_client_)) != ZX_OK) {
        return status;
    }

    RawBitmap block_map;
    // Keep the block_map aligned to a block multiple
    if ((status = block_map.Reset(BlockMapBlocks(fs->info_) * kBlobfsBlockBits)) < 0) {
        fprintf(stderr, "blobfs: Could not reset block bitmap\n");
        return status;
    } else if ((status = block_map.Shrink(fs->info_.data_block_count)) < 0) {
        fprintf(stderr, "blobfs: Could not shrink block bitmap\n");
        return status;
    }
    fzl::ResizeableVmoMapper node_map;

    size_t nodemap_size = kBlobfsInodeSize * fs->info_.inode_count;
    ZX_DEBUG_ASSERT(fbl::round_up(nodemap_size, kBlobfsBlockSize) == nodemap_size);
    ZX_DEBUG_ASSERT(nodemap_size / kBlobfsBlockSize == NodeMapBlocks(fs->info_));
    if ((status = node_map.CreateAndMap(nodemap_size, "nodemap")) != ZX_OK) {
        return status;
    }
    fs->allocator_ = fbl::make_unique<Allocator>(fs.get(), std::move(block_map),
                                                 std::move(node_map));
    if ((status = fs->allocator_->ResetFromStorage(fs::ReadTxn(fs.get()))) != ZX_OK) {
        fprintf(stderr, "blobfs: Failed to load bitmaps: %d\n", status);
        return status;
    }

    if ((status = fs->info_mapping_.CreateAndMap(kBlobfsBlockSize,
                                                 "blobfs-superblock")) != ZX_OK) {
        fprintf(stderr, "blobfs: Failed to create info vmo: %d\n", status);
        return status;
    } else if ((status = fs->AttachVmo(fs->info_mapping_.vmo(),
                                       &fs->info_vmoid_)) != ZX_OK) {
        fprintf(stderr, "blobfs: Failed to attach info vmo: %d\n", status);
        return status;
    } else if ((status = fs->CreateFsId()) != ZX_OK) {
        fprintf(stderr, "blobfs: Failed to create fs_id: %d\n", status);
        return status;
    } else if ((status = fs->InitializeVnodes() != ZX_OK)) {
        fprintf(stderr, "blobfs: Failed to initialize Vnodes\n");
        return status;
    }

    status = Journal::Create(fs.get(), JournalBlocks(fs->info_), JournalStartBlock(fs->info_),
                             &fs->journal_);
    if (status != ZX_OK) {
        return status;
    }

    *out = std::move(fs);
    return ZX_OK;
}

zx_status_t Blobfs::InitializeVnodes() {
    fbl::AutoLock lock(&hash_lock_);
    closed_hash_.clear();
    for (uint32_t i = 0; i < info_.inode_count; ++i) {
        const Inode* inode = GetNode(i);
        if (inode->header.IsAllocated() && !inode->header.IsExtentContainer()) {
            fbl::AllocChecker ac;
            Digest digest(inode->merkle_root_hash);
            fbl::RefPtr<VnodeBlob> vn = fbl::AdoptRef(new (&ac) VnodeBlob(this, digest));
            if (!ac.check()) {
                return ZX_ERR_NO_MEMORY;
            }
            vn->SetState(kBlobStateReadable);
            vn->PopulateInode(i);

            // Delay reading any data from disk until read.
            size_t size = vn->SizeData();
            zx_status_t status = VnodeInsertClosedLocked(std::move(vn));
            if (status != ZX_OK) {
                char name[digest::Digest::kLength * 2 + 1];
                digest.ToString(name, sizeof(name));
                fprintf(stderr, "blobfs: CORRUPTED FILESYSTEM: Duplicate node: "
                        "%s @ index %u\n", name, i);
                return status;
            }
            UpdateLookupMetrics(size);
        }
    }
    return ZX_OK;
}

void Blobfs::VnodeReleaseHard(VnodeBlob* vn) {
    fbl::AutoLock lock(&hash_lock_);
    ZX_ASSERT(open_hash_.erase(vn->GetKey()) != nullptr);
}

void Blobfs::VnodeReleaseSoft(VnodeBlob* raw_vn) {
    fbl::AutoLock lock(&hash_lock_);
    raw_vn->ResurrectRef();
    fbl::RefPtr<VnodeBlob> vn = fbl::internal::MakeRefPtrNoAdopt(raw_vn);
    ZX_ASSERT(open_hash_.erase(raw_vn->GetKey()) != nullptr);
    ZX_ASSERT(VnodeInsertClosedLocked(std::move(vn)) == ZX_OK);
}

zx_status_t Blobfs::Reload() {
    TRACE_DURATION("blobfs", "Blobfs::Reload");

    // Re-read the info block from disk.
    zx_status_t status;
    char block[kBlobfsBlockSize];
    if ((status = readblk(Fd(), 0, block)) != ZX_OK) {
        fprintf(stderr, "blobfs: could not read info block\n");
        return status;
    }

    Superblock* info = reinterpret_cast<Superblock*>(&block[0]);
    if ((status = CheckSuperblock(info, TotalBlocks(*info))) != ZX_OK) {
        fprintf(stderr, "blobfs: Check info failure\n");
        return status;
    }

    // Once it has been verified, overwrite the current info.
    memcpy(&info_, info, sizeof(Superblock));

    // Ensure the block and node maps are up-to-date with changes in size that
    // might have happened.
    status = allocator_->ResetBlockMapSize();
    if (status != ZX_OK) {
        return status;
    }
    status = allocator_->ResetNodeMapSize();
    if (status != ZX_OK) {
        return status;
    }

    // Load the bitmaps from disk.
    if ((status = allocator_->ResetFromStorage(fs::ReadTxn(this))) != ZX_OK) {
        fprintf(stderr, "blobfs: Failed to load bitmaps: %d\n", status);
        return status;
    }

    // Load the vnodes from disk.
    if ((status = InitializeVnodes() != ZX_OK)) {
        fprintf(stderr, "blobfs: Failed to initialize Vnodes\n");
        return status;
    }

    return ZX_OK;
}

zx_status_t Blobfs::VnodeInsertClosedLocked(fbl::RefPtr<VnodeBlob> vn) {
    // To exist in the closed_hash_, this RefPtr must be leaked.
    if (!closed_hash_.insert_or_find(vn.get())) {
        // Set blob state to "Purged" so we do not try to add it to the cached map on recycle.
        vn->SetState(kBlobStatePurged);
        return ZX_ERR_ALREADY_EXISTS;
    }

    // While in the closed cache, the blob may either be destroyed or in an
    // inactive state. The toggles here make tradeoffs between memory usage
    // and performance.
    switch (cache_policy_) {
    case CachePolicy::EvictImmediately:
        vn->TearDown();
        break;
    case CachePolicy::NeverEvict:
        break;
    default:
        ZX_ASSERT_MSG(false, "Unexpected cache policy");
    }

    __UNUSED auto leak = vn.leak_ref();
    return ZX_OK;
}

fbl::RefPtr<VnodeBlob> Blobfs::VnodeUpgradeLocked(const uint8_t* key) {
    ZX_DEBUG_ASSERT(open_hash_.find(key).CopyPointer() == nullptr);
    VnodeBlob* raw_vn = closed_hash_.erase(key);
    if (raw_vn == nullptr) {
        return nullptr;
    }
    open_hash_.insert(raw_vn);
    // To have existed in the closed_hash_, this RefPtr must have
    // been leaked.
    return fbl::internal::MakeRefPtrNoAdopt(raw_vn);
}

zx_status_t Blobfs::OpenRootNode(fbl::RefPtr<VnodeBlob>* out) {
    fbl::AllocChecker ac;
    fbl::RefPtr<VnodeBlob> vn =
        fbl::AdoptRef(new (&ac) VnodeBlob(this));

    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status = vn->Open(0, nullptr);
    if (status != ZX_OK) {
        return status;
    }

    *out = std::move(vn);
    return ZX_OK;
}

zx_status_t Initialize(fbl::unique_fd blockfd, const MountOptions& options,
                       fbl::unique_ptr<Blobfs>* out) {
    zx_status_t status;

    char block[kBlobfsBlockSize];
    if ((status = readblk(blockfd.get(), 0, (void*)block)) < 0) {
        fprintf(stderr, "blobfs: could not read info block\n");
        return status;
    }

    Superblock* info = reinterpret_cast<Superblock*>(&block[0]);

    uint64_t blocks;
    if ((status = GetBlockCount(blockfd.get(), &blocks)) != ZX_OK) {
        fprintf(stderr, "blobfs: cannot find end of underlying device\n");
        return status;
    }

    if ((status = CheckSuperblock(info, blocks)) != ZX_OK) {
        fprintf(stderr, "blobfs: Info check failed\n");
        return status;
    }

    if ((status = Blobfs::Create(std::move(blockfd), options, info, out)) != ZX_OK) {
        fprintf(stderr, "blobfs: mount failed; could not create blobfs\n");
        return status;
    }
    return ZX_OK;
}

zx_status_t Mount(async_dispatcher_t* dispatcher, fbl::unique_fd blockfd,
                  const MountOptions& options, zx::channel root,
                  fbl::Closure on_unmount) {
    zx_status_t status;
    fbl::unique_ptr<Blobfs> fs;

    if ((status = Initialize(std::move(blockfd), options, &fs)) != ZX_OK) {
        return status;
    }

    // Attempt to initialize writeback and journal.
    // The journal must be replayed before the FVM check, in case changes to slice counts have
    // been written to the journal but not persisted to the super block.
    if ((status = fs->InitializeWriteback(options)) != ZX_OK) {
        return status;
    }

    if ((status = CheckFvmConsistency(&fs->Info(), fs->Fd())) != ZX_OK) {
        fprintf(stderr, "blobfs: FVM info check failed\n");
        return status;
    }

    fs->SetDispatcher(dispatcher);
    fs->SetUnmountCallback(std::move(on_unmount));

    fbl::RefPtr<VnodeBlob> vn;
    if ((status = fs->OpenRootNode(&vn)) != ZX_OK) {
        fprintf(stderr, "blobfs: mount failed; could not get root blob\n");
        return status;
    }

    if ((status = fs->ServeDirectory(std::move(vn), std::move(root))) != ZX_OK) {
        fprintf(stderr, "blobfs: mount failed; could not serve root directory\n");
        return status;
    }

    // Shutdown is now responsible for deleting the Blobfs object.
    __UNUSED auto r = fs.release();
    return ZX_OK;
}

zx_status_t Blobfs::CreateWork(fbl::unique_ptr<WritebackWork>* out, VnodeBlob* vnode) {
    if (writeback_ == nullptr) {
        // Transactions should never be allowed if the writeback queue is disabled.
        return ZX_ERR_BAD_STATE;
    }

    out->reset(new WritebackWork(this, fbl::WrapRefPtr(vnode)));
    return ZX_OK;
}

zx_status_t Blobfs::EnqueueWork(fbl::unique_ptr<WritebackWork> work, EnqueueType type) {
    switch (type) {
    case EnqueueType::kJournal:
        if (journal_ != nullptr) {
            // If journaling is enabled (both in general and for this WritebackWork),
            // attempt to enqueue to the journal buffer.
            return journal_->Enqueue(std::move(work));
        }
        // Even if our enqueue type is kJournal,
        // fall through to the writeback queue if the journal doesn't exist.
        __FALLTHROUGH;
    case EnqueueType::kData:
        if (writeback_ != nullptr) {
            return writeback_->Enqueue(std::move(work));
        }
        // If writeback_ does not exist, we are in a readonly state.
        // Fall through to the default case.
        __FALLTHROUGH;
    default:
        // The file system is currently in a readonly state.
        // Reset the work to ensure that any callbacks are completed.
        work->Reset(ZX_ERR_BAD_STATE);
        return ZX_ERR_BAD_STATE;
    }
}
} // namespace blobfs
