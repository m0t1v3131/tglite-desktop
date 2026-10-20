/*
Plume — свои функции поверх Telegram Desktop: встроенный VPN по подписке StealthSurf
(xray рядом с программой + обычный прокси Telegram) и обновления со своего сервера.
Licensed under GNU GPL v. 3.
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace TgLite {

// Номер сборки (ггммддчч); подставляет скрипт сборки. По нему работает обновление.
inline constexpr auto kBuild = 26102016;

void Start();
void Stop();
void CheckUpdates(bool manual);
[[nodiscard]] bool HideStories(); // по умолчанию да, меняется в окне Plume, действует после перезапуска
[[nodiscard]] bool HideRecentSearch(); // по умолчанию да: в пустом поиске нет «Недавних» и частых собеседников
void ShowSettingsBox(not_null<Window::SessionController*> controller);

} // namespace TgLite
