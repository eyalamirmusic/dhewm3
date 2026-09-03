include(CPM)

CPMAddPackage(
	NAME eacp
	GITHUB_REPOSITORY eyalamirmusic/eacp
	GIT_TAG develop
	OPTIONS
		"EACP_BUILD_WEBVIEW OFF")