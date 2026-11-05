from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
old = '''        if(showOverlayMode <= 2) {
            const float overlayWidth = width * 0.6f;
            const float overlayHeight = ofGetHeight() * 0.6f;
            showOverlay->draw((width - overlayWidth) * 0.5f,
                              (ofGetHeight() - overlayHeight) * 0.5f,
                              overlayWidth, overlayHeight);
        } else {
'''
new = '''        if(showOverlayMode == 1) {
            const float overlayWidth = width * 1.2f;
            const float overlayHeight = overlayWidth * showOverlay->getHeight() / showOverlay->getWidth();
            showOverlay->draw((width - overlayWidth) * 0.5f,
                              (ofGetHeight() - overlayHeight) * 0.5f,
                              overlayWidth, overlayHeight);
        } else if(showOverlayMode == 2) {
            const float overlayWidth = width * 0.6f;
            const float overlayHeight = ofGetHeight() * 0.6f;
            showOverlay->draw((width - overlayWidth) * 0.5f,
                              (ofGetHeight() - overlayHeight) * 0.5f,
                              overlayWidth, overlayHeight);
        } else {
'''
if source.count(old) != 1:
    raise SystemExit(f"Expected one overlay branch, found {source.count(old)}")
path.write_text(source.replace(old, new, 1))
