#pragma once

#include "sdkconfig.h"

// Used by GenericDeviceInstanceInfoProvider for the Basic Information
// VendorName and ProductName attributes read by Matter controllers.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME CONFIG_MATTER_VENDOR_NAME
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME CONFIG_MATTER_PRODUCT_NAME
