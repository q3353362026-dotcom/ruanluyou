include $(TOPDIR)/rules.mk

PKG_NAME:=radaer-native
PKG_VERSION:=0.5.12
PKG_RELEASE:=9
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/radaer-native
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Radaer LAN WebUI
  DEPENDS:=+libstdcpp +libopenssl +nftables-json
endef

define Package/radaer-native/description
  Radaer C++ service with LAN-only WebUI, Linux packet capture and replay.
endef

define Package/radaer-native/conffiles
/etc/config/radaer
endef

CMAKE_OPTIONS += -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=MinSizeRel
# musl's small default pthread stack is insufficient for capture buffers + decoding.
TARGET_LDFLAGS += -Wl,-z,stack-size=1048576

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Package/radaer-native/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/usr/share/radaer/webui
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/radaer-native $(1)/usr/bin/
	$(CP) $(PKG_INSTALL_DIR)/usr/share/radaer/webui/. $(1)/usr/share/radaer/webui/
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/usr/libexec
	$(INSTALL_BIN) ./files/radaer.init $(1)/etc/init.d/radaer
	$(INSTALL_CONF) ./files/radaer.config $(1)/etc/config/radaer
	$(INSTALL_BIN) ./files/radaer-run.sh $(1)/usr/libexec/radaer-run
	$(INSTALL_BIN) ./files/radaer-hnat.sh $(1)/usr/libexec/radaer-hnat
endef

$(eval $(call BuildPackage,radaer-native))
