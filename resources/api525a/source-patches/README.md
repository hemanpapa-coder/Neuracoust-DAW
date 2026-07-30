# 킷 소스 패치 보관

`panel.py` — 어택 실크스크린을 오리지널 배치로 수정한 버전(라벨이 자기 점 옆에, 끝점 15µ/15).
`rebuild_panel.py` — 노브 필름스트립 재렌더 없이 패널 배경만 다시 뽑는 스크립트.

재생성: 킷의 `source/` 위에 이 두 파일을 얹고, venv에 `cairosvg pillow numpy scipy shapely`
(+ 시스템 `brew install cairo`) 설치 후:

    DYLD_FALLBACK_LIBRARY_PATH="$(brew --prefix)/lib" python rebuild_panel.py

나온 `assets/2x/panel_background.png`를 `resources/api525a/`로 복사.
