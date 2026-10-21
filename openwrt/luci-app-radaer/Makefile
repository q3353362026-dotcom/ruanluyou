include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-radaer
PKG_VERSION:=0.5.12
PKG_RELEASE:=2

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-radaer
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for Radaer
  PKGARCH:=all
  DEPENDS:=+luci-base +radaer-native +rpcd +libubox +jsonfilter
endef

define Build/Compile
endef

define Package/luci-app-radaer/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/radaer
	$(INSTALL_DATA) ./files/radaer.js $(1)/www/luci-static/resources/view/radaer/overview.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d $(1)/usr/libexec/rpcd
	$(INSTALL_DATA) ./files/menu.json $(1)/usr/share/luci/menu.d/luci-app-radaer.json
	$(INSTALL_DATA) ./files/acl.json $(1)/usr/share/rpcd/acl.d/luci-app-radaer.json
	$(INSTALL_BIN) ./files/rpc.sh $(1)/usr/libexec/rpcd/luci.radaer
endef

define Package/luci-app-radaer/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache /tmp/luci-indexcache.*
	rm -rf /tmp/luci-modulecache
	/etc/init.d/rpcd restart
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-radaer))
