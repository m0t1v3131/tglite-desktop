/*
TG Lite — свои функции поверх Telegram Desktop.
Licensed under GNU GPL v. 3.

VPN: подписка StealthSurf (User-Agent Happ → готовые JSON-настройки xray) → xray(.exe) рядом с программой
слушает SOCKS5 127.0.0.1:kPort → Telegram ходит через него как через обычный прокси.
Разбор подписки — как Sub.kt «Тропы» (m0t1v3131/tropa) и LiteVpn.java Android-версии; менять вместе.

Обновления: https://tg.amdiveev.ru/tglite-desktop.json {"win": {"build", "url"}, "mac": {...}, "notes"}.
*/
#include "tglite/tglite.h"
#include "tglite/tglite_ai.h"

#include "base/call_delayed.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "settings.h"
#include "mtproto/mtproto_proxy_data.h"
#include "storage/localstorage.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QUrlQuery>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TgLite {
namespace {

constexpr auto kPort = 10879;
constexpr auto kUpdateUrl = "https://tg.amdiveev.ru/tglite-desktop.json";
constexpr auto kSubscriptionAgent = "Happ/3.0.0";

struct Entry {
	QString name;
	QJsonObject config;
};

QNetworkAccessManager *Network = nullptr;
QProcess *Xray = nullptr;
QString LastError;

QNetworkAccessManager *Net() {
	if (!Network) {
		Network = new QNetworkAccessManager(QCoreApplication::instance());
	}
	return Network;
}

QString StatePath() {
	return cWorkingDir() + u"tglite.json"_q;
}

QString SubscriptionPath() {
	return cWorkingDir() + u"tglite-sub.txt"_q;
}

QJsonObject ReadState() {
	QFile f(StatePath());
	if (!f.open(QIODevice::ReadOnly)) {
		return {};
	}
	return QJsonDocument::fromJson(f.readAll()).object();
}

void WriteState(const QJsonObject &state) {
	QFile f(StatePath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(QJsonDocument(state).toJson());
	}
}

QString VpnUrl() {
	return ReadState().value(u"vpnUrl"_q).toString();
}

bool VpnEnabled() {
	const auto state = ReadState();
	return state.value(u"vpnEnabled"_q).toBool()
		&& !state.value(u"vpnUrl"_q).toString().isEmpty();
}

int VpnServer() {
	return ReadState().value(u"vpnServer"_q).toInt();
}

void SetState(const QString &key, const QJsonValue &value) {
	auto state = ReadState();
	state.insert(key, value);
	WriteState(state);
}

QByteArray ReadSubscription() {
	QFile f(SubscriptionPath());
	return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void WriteSubscription(const QByteArray &data) {
	QFile f(SubscriptionPath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(data);
	}
}

// ---------- разбор подписки ----------

QJsonObject Stream(const QUrlQuery &q, const QString &host) {
	const auto get = [&](const QString &key, const QString &def) {
		return q.hasQueryItem(key)
			? q.queryItemValue(key, QUrl::FullyDecoded)
			: def;
	};
	auto s = QJsonObject{ { u"network"_q, get(u"type"_q, u"tcp"_q) } };
	const auto security = get(u"security"_q, QString());
	if (security == u"reality"_q) {
		s.insert(u"security"_q, u"reality"_q);
		s.insert(u"realitySettings"_q, QJsonObject{
			{ u"serverName"_q, get(u"sni"_q, QString()) },
			{ u"publicKey"_q, get(u"pbk"_q, QString()) },
			{ u"shortId"_q, get(u"sid"_q, QString()) },
			{ u"fingerprint"_q, get(u"fp"_q, u"chrome"_q) },
			{ u"spiderX"_q, get(u"spx"_q, u"/"_q) },
		});
	} else if (security == u"tls"_q) {
		auto tls = QJsonObject{ { u"serverName"_q, get(u"sni"_q, host) } };
		if (q.hasQueryItem(u"fp"_q)) {
			tls.insert(u"fingerprint"_q, get(u"fp"_q, QString()));
		}
		if (q.hasQueryItem(u"alpn"_q)) {
			tls.insert(
				u"alpn"_q,
				QJsonArray::fromStringList(get(u"alpn"_q, QString()).split(',')));
		}
		s.insert(u"security"_q, u"tls"_q);
		s.insert(u"tlsSettings"_q, tls);
	}
	const auto type = get(u"type"_q, QString());
	if (type == u"grpc"_q) {
		s.insert(u"grpcSettings"_q, QJsonObject{
			{ u"serviceName"_q, get(u"serviceName"_q, QString()) },
		});
	} else if (type == u"ws"_q) {
		auto ws = QJsonObject{ { u"path"_q, get(u"path"_q, u"/"_q) } };
		if (q.hasQueryItem(u"host"_q)) {
			ws.insert(u"headers"_q, QJsonObject{
				{ u"Host"_q, get(u"host"_q, QString()) },
			});
		}
		s.insert(u"wsSettings"_q, ws);
	} else if (type == u"xhttp"_q) {
		auto x = QJsonObject{
			{ u"path"_q, get(u"path"_q, u"/"_q) },
			{ u"mode"_q, get(u"mode"_q, u"auto"_q) },
		};
		if (q.hasQueryItem(u"host"_q)) {
			x.insert(u"host"_q, get(u"host"_q, QString()));
		}
		s.insert(u"xhttpSettings"_q, x);
	}
	return s;
}

std::optional<Entry> LinkToEntry(QString link) {
	if (link.startsWith(u"hy2:"_q, Qt::CaseInsensitive)) {
		link = u"hysteria2:"_q + link.mid(4);
	}
	const auto url = QUrl(link);
	const auto q = QUrlQuery(url);
	const auto host = url.host();
	if (host.isEmpty()) {
		return std::nullopt;
	}
	const auto port = url.port(443);
	const auto user = url.userName(QUrl::FullyDecoded);
	const auto name = url.fragment(QUrl::FullyDecoded).isEmpty()
		? host
		: url.fragment(QUrl::FullyDecoded);
	const auto scheme = url.scheme().toLower();
	auto outbound = QJsonObject{ { u"tag"_q, u"proxy"_q } };
	if (scheme == u"vless"_q) {
		auto vu = QJsonObject{
			{ u"id"_q, user },
			{ u"encryption"_q, q.hasQueryItem(u"encryption"_q)
				? q.queryItemValue(u"encryption"_q)
				: u"none"_q },
		};
		if (!q.queryItemValue(u"flow"_q).isEmpty()) {
			vu.insert(u"flow"_q, q.queryItemValue(u"flow"_q));
		}
		outbound.insert(u"protocol"_q, u"vless"_q);
		outbound.insert(u"settings"_q, QJsonObject{ { u"vnext"_q, QJsonArray{
			QJsonObject{
				{ u"address"_q, host },
				{ u"port"_q, port },
				{ u"users"_q, QJsonArray{ vu } },
			},
		} } });
		outbound.insert(u"streamSettings"_q, Stream(q, host));
	} else if (scheme == u"trojan"_q) {
		auto tq = q;
		if (!tq.hasQueryItem(u"security"_q)) {
			tq.addQueryItem(u"security"_q, u"tls"_q);
		}
		outbound.insert(u"protocol"_q, u"trojan"_q);
		outbound.insert(u"settings"_q, QJsonObject{ { u"servers"_q, QJsonArray{
			QJsonObject{
				{ u"address"_q, host },
				{ u"port"_q, port },
				{ u"password"_q, user },
			},
		} } });
		outbound.insert(u"streamSettings"_q, Stream(tq, host));
	} else if (scheme == u"hysteria2"_q) {
		outbound.insert(u"protocol"_q, u"hysteria"_q);
		outbound.insert(u"settings"_q, QJsonObject{
			{ u"address"_q, host },
			{ u"port"_q, port },
			{ u"version"_q, 2 },
		});
		outbound.insert(u"streamSettings"_q, QJsonObject{
			{ u"network"_q, u"hysteria"_q },
			{ u"hysteriaSettings"_q, QJsonObject{
				{ u"version"_q, 2 },
				{ u"auth"_q, user },
			} },
			{ u"security"_q, u"tls"_q },
			{ u"tlsSettings"_q, QJsonObject{
				{ u"serverName"_q, q.hasQueryItem(u"sni"_q)
					? q.queryItemValue(u"sni"_q)
					: host },
				{ u"alpn"_q, QJsonArray{ u"h3"_q } },
			} },
		});
	} else {
		return std::nullopt;
	}
	const auto config = QJsonObject{
		{ u"outbounds"_q, QJsonArray{
			outbound,
			QJsonObject{ { u"tag"_q, u"direct"_q }, { u"protocol"_q, u"freedom"_q } },
		} },
	};
	return Entry{ name, config };
}

std::vector<Entry> Parse(const QByteArray &data) {
	auto result = std::vector<Entry>();
	const auto text = QString::fromUtf8(data).trimmed();
	if (text.startsWith('[') || text.startsWith('{')) {
		const auto doc = QJsonDocument::fromJson(text.toUtf8());
		const auto list = doc.isArray()
			? doc.array()
			: QJsonArray{ doc.object() };
		for (auto i = 0; i != list.size(); ++i) {
			const auto config = list[i].toObject();
			if (!config.value(u"outbounds"_q).isArray()) {
				continue;
			}
			const auto name = config.value(u"remarks"_q).toString();
			result.push_back({
				name.isEmpty() ? (u"Сервер "_q + QString::number(i + 1)) : name,
				config,
			});
		}
		return result;
	}
	auto links = text;
	if (!QRegularExpression(u"^[a-z0-9]+://"_q, QRegularExpression::CaseInsensitiveOption).match(links).hasMatch()) {
		links = QString::fromUtf8(QByteArray::fromBase64(
			text.toUtf8(),
			QByteArray::Base64UrlEncoding | QByteArray::Base64Encoding));
	}
	for (const auto &line : links.split('\n')) {
		if (auto entry = LinkToEntry(line.trimmed())) {
			result.push_back(std::move(*entry));
		}
	}
	return result;
}

QStringList ServerHosts(const QJsonObject &config) {
	auto result = QStringList();
	for (const auto &value : config.value(u"outbounds"_q).toArray()) {
		const auto settings = value.toObject().value(u"settings"_q).toObject();
		auto addresses = QStringList{ settings.value(u"address"_q).toString() };
		for (const auto key : { u"vnext"_q, u"servers"_q }) {
			for (const auto &server : settings.value(key).toArray()) {
				addresses.push_back(server.toObject().value(u"address"_q).toString());
			}
		}
		for (const auto &address : addresses) {
			if (!address.isEmpty()
				&& !QRegularExpression(u"^[\\d.]+$"_q).match(address).hasMatch()
				&& !address.contains(':')) {
				result.push_back(u"full:"_q + address);
			}
		}
	}
	result.removeDuplicates();
	return result;
}

// Настройки сервера как есть; меняем только вход (SOCKS5 для Telegram) и DNS — как у «Тропы».
QByteArray SocksConfig(const Entry &entry) {
	auto c = entry.config;
	c.remove(u"remarks"_q);
	c.insert(u"log"_q, QJsonObject{ { u"loglevel"_q, u"warning"_q } });
	c.insert(u"inbounds"_q, QJsonArray{ QJsonObject{
		{ u"tag"_q, u"socks-in"_q },
		{ u"protocol"_q, u"socks"_q },
		{ u"listen"_q, u"127.0.0.1"_q },
		{ u"port"_q, kPort },
		{ u"settings"_q, QJsonObject{
			{ u"auth"_q, u"noauth"_q },
			{ u"udp"_q, true },
		} },
	} });
	auto local = QStringList{
		u"domain:ru"_q,
		u"domain:su"_q,
		u"domain:xn--p1ai"_q,
		u"domain:stealthsurf.net"_q,
		u"domain:stealthsurf.app"_q,
	} + ServerHosts(c);
	c.insert(u"dns"_q, QJsonObject{
		{ u"queryStrategy"_q, u"UseIPv4"_q },
		{ u"servers"_q, QJsonArray{
			QJsonObject{
				{ u"address"_q, u"77.88.8.8"_q },
				{ u"domains"_q, QJsonArray::fromStringList(local) },
			},
			u"https://1.1.1.1/dns-query"_q,
		} },
	});
	return QJsonDocument(c).toJson(QJsonDocument::Compact);
}

// ---------- xray и прокси Telegram ----------

MTP::ProxyData OurProxy() {
	auto proxy = MTP::ProxyData();
	proxy.type = MTP::ProxyData::Type::Socks5;
	proxy.host = u"127.0.0.1"_q;
	proxy.port = kPort;
	return proxy;
}

void ApplyProxy(bool enabled) {
	auto &settings = Core::App().settings().proxy();
	const auto proxy = OurProxy();
	if (enabled) {
		if (ranges::find(settings.list(), proxy) == end(settings.list())) {
			settings.addToList(proxy);
		}
		Core::App().setCurrentProxy(proxy, MTP::ProxyData::Settings::Enabled);
	} else if (settings.selected() == proxy) {
		Core::App().setCurrentProxy(proxy, MTP::ProxyData::Settings::System);
	}
	Local::writeSettings();
}

QString XrayPath() {
#ifdef Q_OS_WIN
	return QCoreApplication::applicationDirPath() + u"/xray.exe"_q;
#else // Q_OS_WIN
	return QCoreApplication::applicationDirPath() + u"/xray"_q;
#endif // Q_OS_WIN
}

void StopXray() {
	if (Xray) {
		Xray->kill();
		Xray->waitForFinished(2000);
		delete Xray;
		Xray = nullptr;
	}
}

bool StartXray() {
	StopXray();
	const auto list = Parse(ReadSubscription());
	if (list.empty()) {
		LastError = u"в подписке нет серверов"_q;
		return false;
	}
	auto index = VpnServer();
	if (index < 0 || index >= int(list.size())) {
		index = 0;
	}
	const auto configPath = cWorkingDir() + u"tglite-xray.json"_q;
	QFile f(configPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		LastError = u"не удалось записать настройки xray"_q;
		return false;
	}
	f.write(SocksConfig(list[index]));
	f.close();
	if (!QFile::exists(XrayPath())) {
		LastError = u"нет файла xray рядом с программой"_q;
		return false;
	}
	Xray = new QProcess();
	Xray->setProgram(XrayPath());
	Xray->setArguments({ u"run"_q, u"-c"_q, configPath });
	Xray->setWorkingDirectory(QCoreApplication::applicationDirPath());
	Xray->setStandardOutputFile(cWorkingDir() + u"tglite-xray.log"_q);
	Xray->setStandardErrorFile(cWorkingDir() + u"tglite-xray.log"_q, QIODevice::Append);
	Xray->start();
	if (!Xray->waitForStarted(5000)) {
		LastError = u"xray не запустился"_q;
		StopXray();
		return false;
	}
	LastError = QString();
	ApplyProxy(true);
	return true;
}

void FetchSubscription(Fn<void(QString error)> done) {
	auto request = QNetworkRequest(QUrl(VpnUrl()));
	request.setHeader(QNetworkRequest::UserAgentHeader, QString(kSubscriptionAgent));
	request.setTransferTimeout(20000);
	const auto reply = Net()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			done(u"подписка не скачалась: "_q + reply->errorString());
			return;
		}
		const auto data = reply->readAll();
		if (Parse(data).empty()) {
			done(u"в подписке нет серверов"_q);
			return;
		}
		WriteSubscription(data);
		done(QString());
	});
}

// ---------- обновления ----------

void InstallUpdate(const QString &url) {
	const auto window = Core::App().activePrimaryWindow();
	if (window) {
		window->showToast(u"Скачиваю обновление Plume…"_q);
	}
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto reply = Net()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto data = reply->readAll();
		if (reply->error() != QNetworkReply::NoError || data.size() < 1024 * 1024) {
			if (const auto window = Core::App().activePrimaryWindow()) {
				window->showToast(u"Не удалось скачать обновление"_q);
			}
			return;
		}
		const auto dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
		// Новый Telegram.exe кладём рядом, а заменяет его скрипт после выхода программы.
		const auto exe = QCoreApplication::applicationFilePath();
		const auto fresh = exe + u".new"_q;
		QFile f(fresh);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}
		f.write(data);
		f.close();
		const auto script = QDir::tempPath() + u"/tglite-update.cmd"_q;
		QFile s(script);
		if (!s.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}
		const auto native = [](const QString &path) {
			return QDir::toNativeSeparators(path);
		};
		s.write(QString(
			"@echo off\r\n"
			"timeout /t 2 /nobreak >nul\r\n"
			":wait\r\n"
			"move /y \"%1\" \"%2\" >nul 2>&1 || (timeout /t 1 /nobreak >nul & goto wait)\r\n"
			"start \"\" \"%2\"\r\n"
			"del \"%~f0\"\r\n").arg(native(fresh), native(exe)).toLocal8Bit());
		s.close();
		QProcess::startDetached(u"cmd.exe"_q, { u"/c"_q, native(script) });
#else // Q_OS_WIN
		// Mac: архив .app распаковывается поверх, затем программа перезапускается.
		const auto zip = QDir::tempPath() + u"/tglite-update.zip"_q;
		QFile f(zip);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}
		f.write(data);
		f.close();
		auto app = QDir(dir);
		app.cdUp();
		app.cdUp();
		const auto bundle = app.absolutePath();
		const auto parent = QFileInfo(bundle).absolutePath();
		const auto script = QString(
			"sleep 2; ditto -x -k '%1' '%2' && xattr -cr '%3'; open '%3'"
		).arg(zip, parent, bundle);
		QProcess::startDetached(u"/bin/sh"_q, { u"-c"_q, script });
#endif // Q_OS_WIN
		Core::Quit();
	});
}

void OfferUpdate(const QString &url, const QString &notes) {
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		return;
	}
	window->show(Ui::MakeConfirmBox({
		.text = u"Доступна новая версия Plume."_q
			+ (notes.isEmpty() ? QString() : (u"\n\n"_q + notes)),
		.confirmed = [=](Fn<void()> close) { close(); InstallUpdate(url); },
		.confirmText = u"Обновить"_q,
		.cancelText = u"Позже"_q,
	}));
}

} // namespace

bool HideStories() {
	const auto state = ReadState();
	return !state.contains(u"hideStories"_q)
		|| state.value(u"hideStories"_q).toBool();
}

void CheckUpdates(bool manual) {
	auto request = QNetworkRequest(QUrl(QString(kUpdateUrl)));
	request.setTransferTimeout(15000);
	const auto reply = Net()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto info = QJsonDocument::fromJson(reply->readAll()).object();
#ifdef Q_OS_WIN
		const auto platform = info.value(u"win"_q).toObject();
#else // Q_OS_WIN
		const auto platform = info.value(u"mac"_q).toObject();
#endif // Q_OS_WIN
		const auto build = platform.value(u"build"_q).toInt();
		const auto window = Core::App().activePrimaryWindow();
		if (reply->error() != QNetworkReply::NoError || !build) {
			if (manual && window) {
				window->showToast(u"Не удалось проверить обновления"_q);
			}
		} else if (build > kBuild) {
			OfferUpdate(
				platform.value(u"url"_q).toString(),
				info.value(u"notes"_q).toString());
		} else if (manual && window) {
			window->showToast(u"У вас последняя версия Plume"_q);
		}
	});
}

void Start() {
	if (VpnEnabled()) {
		if (!StartXray()) {
			// В кэше нет подписки или она испорчена — скачиваем заново.
			FetchSubscription([](QString error) {
				if (error.isEmpty()) {
					StartXray();
				} else {
					LastError = error;
				}
			});
		} else {
			FetchSubscription([](QString) {});
		}
	}
	StartCommandPolling();
	base::call_delayed(10000, QCoreApplication::instance(), [] {
		CheckUpdates(false);
	});
}

void Stop() {
	StopXray();
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Plume"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"VPN StealthSurf: Telegram работает через вашу подписку, остальные программы не затрагиваются. "
			u"Ссылка — в личном кабинете StealthSurf или в Happ."_q,
			st::boxLabel));
		const auto url = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Ссылка на подписку"_q),
			VpnUrl()));
		const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Telegram через VPN"_q,
			VpnEnabled()));
		const auto list = Parse(ReadSubscription());
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			Xray
				? (u"Работает, сервер: "_q + ((VpnServer() < int(list.size()))
					? list[VpnServer()].name
					: QString()))
				: (LastError.isEmpty() ? u"VPN выключен"_q : (u"Ошибка: "_q + LastError)),
			st::boxDividerLabel));
		if (!list.empty()) {
			box->addRow(object_ptr<Ui::LinkButton>(
				box,
				u"Выбрать сервер"_q))->setClickedCallback([=] {
				auto names = std::vector<QString>();
				for (const auto &entry : list) {
					names.push_back(entry.name);
				}
				controller->show(Box([=](not_null<Ui::GenericBox*> choose) {
					SingleChoiceBox(choose, {
						.title = rpl::single(u"Сервер"_q),
						.options = names,
						.initialSelection = std::min(VpnServer(), int(names.size()) - 1),
						.callback = [=](int index) {
							SetState(u"vpnServer"_q, index);
							if (VpnEnabled()) {
								StartXray();
							}
						},
					});
				}));
			});
		}
		const auto stories = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Скрыть истории (после перезапуска)"_q,
			HideStories()));
		stories->checkedChanges() | rpl::on_next([](bool checked) {
			SetState(u"hideStories"_q, checked);
		}, stories->lifetime());
		box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Проверить обновления"_q))->setClickedCallback([] {
			CheckUpdates(true);
		});
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto link = url->getLastText().trimmed();
			const auto on = enabled->checked() && !link.isEmpty();
			const auto changed = (link != VpnUrl());
			SetState(u"vpnUrl"_q, link);
			SetState(u"vpnEnabled"_q, on);
			if (!on) {
				StopXray();
				ApplyProxy(false);
				box->closeBox();
				return;
			}
			status->setText(u"Подключаю…"_q);
			const auto start = [=] {
				if (StartXray()) {
					controller->showToast(u"VPN включён"_q);
					box->closeBox();
				} else {
					status->setText(u"Ошибка: "_q + LastError);
				}
			};
			if (changed || ReadSubscription().isEmpty()) {
				FetchSubscription([=](QString error) {
					if (error.isEmpty()) {
						start();
					} else {
						LastError = error;
						status->setText(u"Ошибка: "_q + error);
					}
				});
			} else {
				start();
			}
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace TgLite
