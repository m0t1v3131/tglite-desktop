/*
TG Lite — свои функции поверх Telegram Desktop: встроенный VPN по подписке StealthSurf
(xray рядом с программой + обычный прокси Telegram) и обновления со своего сервера.
Licensed under GNU GPL v. 3.
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace TgLite {

// Номер сборки (ггммддчч); подставляет скрипт сборки. По нему работает обновление.
inline constexpr auto kBuild = 26101911;

void Start();
void Stop();
void CheckUpdates(bool manual);
[[nodiscard]] bool HideStories(); // по умолчанию да, меняется в окне Plume, действует после перезапуска
void ShowSettingsBox(not_null<Window::SessionController*> controller);

} // namespace TgLite
