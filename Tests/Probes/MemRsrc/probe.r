/* Resources the MemRsrc probe reads back from its own resource fork. */

data 'PRBs' (128, "First") {
	$"0102 0304"
};

data 'PRBs' (129, "Second", purgeable) {
	$"0A0B 0C0D 0E0F"
};

data 'PRBs' (130) {
	$"1122 3344 5566 7788"
};

data 'PRBs' (131, "Lazy") {
	$"AA"
};

data 'STR ' (128) {
	$"0C" "probe string"
};
