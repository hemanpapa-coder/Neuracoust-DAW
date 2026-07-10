#include "core/Localization.h"

#include <cassert>

using namespace neuracoust::daw;

int main() {
    assert(uiLanguageFromLocaleTag("ko-KR") == UiLanguage::Korean);
    assert(uiLanguageFromLocaleTag("en-US") == UiLanguage::English);
    assert(uiLanguageFromLocaleTag("ja_JP") == UiLanguage::Japanese);
    assert(uiLanguageFromLocaleTag("zh-Hans-CN") == UiLanguage::ChineseSimplified);
    assert(uiLanguageFromLocaleTag("fr-FR") == UiLanguage::English);

    setUiLanguageFromLocaleTag("ko-KR");
    assert(tr("ai.title") == "AI 어시스턴트");
    setUiLanguageFromLocaleTag("ja-JP");
    assert(tr("common.close") == "閉じる");
    assert(tr("ai.button.queue") == "作業キュー");
    setUiLanguageFromLocaleTag("zh-CN");
    assert(tr("ai.button.apply") == "应用");
    assert(tr("ai.status.ready").find("本地 AI") != std::string::npos);
    setUiLanguageFromLocaleTag("en-US");
    assert(tr("ai.button.apply") == "Apply");
    assert(tr("missing.key") == "missing.key");

    return 0;
}
