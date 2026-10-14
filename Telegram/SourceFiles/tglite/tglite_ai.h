/*
Plume (TG Lite): ИИ-помощник на компьютере. Модель — через сервер tg.amdiveev.ru (/api/ai),
действия (чаты, папки, сообщения, рассылка) выполняются здесь, сессия Telegram никуда не уходит.
Licensed under GNU GPL v. 3.
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace TgLite {

void ShowAssistant(not_null<Window::SessionController*> controller);

} // namespace TgLite
