PROJECT_NAME := medklinn

EXTRA_COMPONENT_DIRS := $(abspath ../..) $(IDF_PATH)/examples/common_components/qrcode

include $(IDF_PATH)/make/project.mk
