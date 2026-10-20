/*
Plume: ИИ-помощник на компьютере. Модель — через сервер plume.amdiveev.ru (/api/ai),
действия (чаты, папки, сообщения, рассылка) выполняются здесь, сессия Telegram никуда не уходит.
Licensed under GNU GPL v. 3.
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class AbstractButton;
class InputField;
} // namespace Ui

class QWidget;
class History;

namespace TgLite {

void InstallReplySuggestions(
	not_null<Ui::InputField*> field,
	not_null<Window::SessionController*> controller,
	Fn<History*()> currentHistory);

void ShowAssistant(not_null<Window::SessionController*> controller);
void ToggleAssistant(not_null<Window::SessionController*> controller); // Ctrl+T и кнопка «Takt»

// Кнопка «Takt» со счётчиком «ждут вас» (справа от поиска). Ставит Ctrl+T и открывает Takt при старте, если он был открыт.
[[nodiscard]] Ui::AbstractButton *CreateTaktButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// Раз в 30 с забирает задания других проектов (plume.amdiveev.ru/api/cmd-pending) и выполняет их помощником.
void StartCommandPolling();

} // namespace TgLite
