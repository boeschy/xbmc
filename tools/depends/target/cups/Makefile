include ../../Makefile.include CUPS-VERSION ../../download-files.include
DEPS = ../../Makefile.include Makefile CUPS-VERSION ../../download-files.include


all: .installed-$(PLATFORM)

.installed-$(PLATFORM): $(DEPS) | $(TARBALLS_LOCATION)/$(ARCHIVE).$(HASH_TYPE)
	rm -rf $(PLATFORM)/*; mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	cp -rf $(PLATFORM) $(PREFIX)/share/cups
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) clean
	rm -f .installed-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM)
