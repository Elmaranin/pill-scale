#include "board/flash_partition.h"

static const esp_partition_subtype_t LOG_SUBTYPE = (esp_partition_subtype_t)0x40;

bool PartitionFlash::begin() {
    p_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, LOG_SUBTYPE, "log");
    return p_ != nullptr;
}

uint32_t PartitionFlash::size() const { return p_ ? p_->size : 0; }

bool PartitionFlash::read(uint32_t offset, void *buf, size_t len) {
    return p_ && esp_partition_read(p_, offset, buf, len) == ESP_OK;
}

bool PartitionFlash::write(uint32_t offset, const void *buf, size_t len) {
    return p_ && esp_partition_write(p_, offset, buf, len) == ESP_OK;
}

bool PartitionFlash::erase_sector(uint32_t offset) {
    return p_ && esp_partition_erase_range(p_, offset, SECTOR) == ESP_OK;
}
