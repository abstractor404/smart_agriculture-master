
idf_component_register(SRCS "smart_agriculture.c"
                INCLUDE_DIRS "."
                REQUIRES mcommon mconfig mwifi mlink mesh_mqtt_handle sensor
)
