#pragma once
#include "algo/journal.h"
#include "esp_partition.h"

// Flash for the journal: partition "log" from partitions.csv (type data, subtype 0x40).
class PartitionFlash : public Flash {
public:
    bool begin();  // false — no such partition (old partition table)

    uint32_t size() const override;
    bool read(uint32_t offset, void *buf, size_t len) override;
    bool write(uint32_t offset, const void *buf, size_t len) override;
    bool erase_sector(uint32_t offset) override;

private:
    const esp_partition_t *p_ = nullptr;
};
