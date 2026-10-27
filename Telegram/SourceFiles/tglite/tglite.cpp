/*
Plume — свои функции поверх Telegram Desktop.
Licensed under GNU GPL v. 3.

VPN: подписка StealthSurf (User-Agent Happ → готовые JSON-настройки xray) → xray(.exe) рядом с программой
слушает SOCKS5 127.0.0.1:kPort → Telegram ходит через него как через обычный прокси.
Разбор подписки — как Sub.kt «Тропы» (m0t1v3131/tropa) и LiteVpn.java Android-версии; менять вместе.

Обновления: https://plume.amdiveev.ru/tglite-desktop.json {"win": {"build", "url"}, "mac": {...}, "notes"}.
*/
#include "tglite/tglite.h"
#include "tglite/tglite_ai.h"

#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "api/api_updates.h"
#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
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
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_controller.h"
#include "window/notifications_manager.h"
#include "mainwindow.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"
#include "styles/style_basic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QUrlQuery>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TgLite {
namespace {

constexpr auto kPort = 10879;
constexpr auto kUpdateUrl = "https://plume.amdiveev.ru/tglite-desktop.json";
constexpr auto kSubscriptionAgent = "Happ/3.0.0";
constexpr auto kTaktBotId = uint64(8909004776); // бот Такт — свой, из архива не прячем
constexpr auto kMayakBotId = uint64(8957007851); // бот Маяка — тоже свой
constexpr auto kQuietUnread = 30;

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

// Общие настройки на телефоне, ПК и Mac (plume.amdiveev.ru/api/settings/<id>): эти ключи уходят на сервер.
bool ApplyingRemote = false;

bool IsSyncedKey(const QString &key) {
	static const auto keys = base::flat_set<QString>{
		u"hideStories"_q,
		u"hideRecentSearch"_q,
		u"hideArchiveChannels"_q,
		u"hideArchiveBots"_q,
		u"quietGroups"_q,
		u"muteAll"_q,
		u"ghostMode"_q,
		u"focusMode"_q,
		u"focusChats"_q,
		u"hideTaktBot"_q,
		u"hideMayakBot"_q,
	};
	return keys.contains(key);
}

void SetState(const QString &key, const QJsonValue &value) {
	auto state = ReadState();
	state.insert(key, value);
	const auto synced = !ApplyingRemote && IsSyncedKey(key);
	if (synced) {
		state.insert(
			u"settingsUpdated"_q,
			double(QDateTime::currentMSecsSinceEpoch()));
	}
	WriteState(state);
	if (synced) {
		PushSettings();
	}
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
	static bool updating = false;
	if (updating) {
		return;
	}
	const auto main = Core::App().activePrimaryWindow();
	if (!main) {
		return;
	}
	updating = true;
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(60000);
	const auto reply = Net()->get(request);
	const auto canceled = std::make_shared<bool>(false);
	// Окно скачивания — обычное окно Plume (раньше был серый системный QProgressDialog).
	const auto progressBox = main->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Обновление Плюма"_q));
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Скачиваю новую версию…"_q,
			st::boxLabel));
		const auto bar = box->addRow(object_ptr<Ui::RpWidget>(box));
		bar->resize(bar->width(), style::ConvertScale(6));
		const auto share = std::make_shared<float64>(0.);
		bar->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(bar);
			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			const auto radius = bar->height() / 2.;
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(QRectF(bar->rect()), radius, radius);
			if (*share > 0.) {
				const auto filled = std::max(
					float64(bar->height()),
					bar->width() * *share);
				p.setBrush(st::activeButtonBg);
				p.drawRoundedRect(
					QRectF(0., 0., filled, bar->height()),
					radius,
					radius);
			}
		}, bar->lifetime());
		box->addSkip(st::boxLittleSkip);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Потом Плюм закроется, поставит новую версию и откроется снова. Чаты и настройки сохранятся."_q,
			st::boxDividerLabel));
		box->addButton(rpl::single(u"Отмена"_q), [=] {
			// abort() сразу вызывает finished, а тот закрывает и удаляет это окно —
			// поэтому сначала закрыть окно, потом прервать (раньше Плюм падал).
			const auto r = reply;
			*canceled = true;
			box->closeBox();
			r->abort();
		});
		QObject::connect(reply, &QNetworkReply::downloadProgress, label,
			[=](qint64 received, qint64 total) {
				const auto mb = [](qint64 bytes) {
					return QString::number(bytes / 1048576., 'f', 0);
				};
				if (total > 0) {
					*share = std::clamp(float64(received) / total, 0., 1.);
					label->setText(u"Скачано %1 из %2 МБ · %3%"_q
						.arg(mb(received), mb(total))
						.arg(int(*share * 100)));
				} else {
					label->setText(u"Скачано %1 МБ"_q.arg(mb(received)));
				}
				bar->update();
			});
	}), Ui::LayerOption::CloseOther);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		updating = false;
		if (*canceled) {
			reply->deleteLater();
			return;
		} else if (const auto strong = progressBox.get()) {
			strong->closeBox();
		}
		reply->deleteLater();
		if (*canceled) {
			return;
		}
		const auto data = reply->readAll();
		if (reply->error() != QNetworkReply::NoError || data.size() < 1024 * 1024) {
			if (const auto window = Core::App().activePrimaryWindow()) {
				window->showToast(u"Не удалось скачать обновление"_q);
			}
			return;
		}
		const auto window = Core::App().activePrimaryWindow();
		if (!window) {
			return;
		}
		window->show(Ui::MakeConfirmBox({
			.text = u"Обновление скачано.\n\nПлюм закроется, установит новую версию и откроется снова. Чаты и настройки сохранятся."_q,
			.confirmed = [=](Fn<void()> close) {
			close();
			const auto fail = [] {
				if (const auto window = Core::App().activePrimaryWindow()) {
					window->showToast(u"Не удалось запустить установку. Попробуйте обновить ещё раз."_q);
				}
			};
			const auto dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
			// Новый Telegram.exe кладём рядом, а заменяет его скрипт после выхода программы.
			const auto exe = QCoreApplication::applicationFilePath();
			const auto fresh = exe + u".new"_q;
			QFile f(fresh);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				fail();
				return;
			}
			if (f.write(data) != data.size()) {
				fail();
				return;
			}
			f.close();
			const auto script = QDir::tempPath() + u"/tglite-update.cmd"_q;
			QFile s(script);
			if (!s.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				fail();
				return;
			}
			const auto native = [](const QString &path) {
				return QDir::toNativeSeparators(path);
			};
			// Пути передаём аргументами, а не текстом скрипта: cmd читает .cmd в кодировке OEM,
			// и кириллица в пути (C:/Users/Александр/...) ломала move — Plume закрывался без обновления.
			s.write(
				"@echo off\r\n"
				"title Plume Update\r\n"
				"echo Installing Plume update. Please wait...\r\n"
				"timeout /t 2 /nobreak >nul\r\n"
				"set n=0\r\n"
				":wait\r\n"
				"move /y \"%~1\" \"%~2\" >nul 2>&1 && goto run\r\n"
				"set /a n+=1\r\n"
				"if %n% geq 60 goto run\r\n"
				"timeout /t 1 /nobreak >nul\r\n"
				"goto wait\r\n"
				":run\r\n"
				"start \"\" \"%~2\"\r\n"
				"del \"%~f0\"\r\n");
			s.close();
			QProcess process;
			process.setProgram(u"cmd.exe"_q);
			process.setNativeArguments(u"/c \"\"%1\" \"%2\" \"%3\"\""_q.arg(
				native(script),
				native(fresh),
				native(exe)));
			if (!process.startDetached()) {
				fail();
				return;
			}
#else // Q_OS_WIN
			// Mac: архив .app распаковывается во временную папку и заменяет
			// программу целиком, затем Плюм перезапускается. Плюм помечает
			// скачанное карантином (LSFileQuarantineEnabled), а ditto с таким
			// архивом не может распаковать прямо в «Программы» — поэтому
			// карантин снимается заранее, а распаковка идёт не туда.
			const auto zip = QDir::tempPath() + u"/tglite-update.zip"_q;
			QFile f(zip);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				fail();
				return;
			}
			if (f.write(data) != data.size()) {
				fail();
				return;
			}
			f.close();
			auto app = QDir(dir);
			app.cdUp();
			app.cdUp();
			const auto bundle = app.absolutePath();
			const auto script = QString(
				"n=0; while kill -0 \"$3\" 2>/dev/null && [ $n -lt 60 ]; do "
				"sleep 1; n=$((n+1)); done; "
				"xattr -c \"$1\"; "
				"t=$(mktemp -d /tmp/plume-update.XXXXXX) || exit 1; "
				"if ditto -x -k \"$1\" \"$t\" && [ -d \"$t/Plume.app\" ]; then "
				"xattr -cr \"$t/Plume.app\"; "
				"if mv \"$2\" \"$t/old.app\"; then "
				"mv \"$t/Plume.app\" \"$2\" || mv \"$t/old.app\" \"$2\"; "
				"else ditto \"$t/Plume.app\" \"$2\"; fi; "
				"xattr -cr \"$2\"; "
				"fi; "
				"open \"$2\"; "
				"rm -rf \"$t\" \"$1\"");
			QProcess::startDetached(
				u"/bin/sh"_q,
				{
					u"-c"_q,
					script,
					u"plume-update"_q,
					zip,
					bundle,
					QString::number(QCoreApplication::applicationPid()),
				});
#endif // Q_OS_WIN
			Core::Quit();
			},
			.confirmText = u"Установить и перезапустить"_q,
			.cancelText = u"Позже"_q,
		}));
	});
}

// Шторка остаётся поверх чата, пока владелец не выберет действие.
class UpdateCurtain final : public QFrame {
public:
	UpdateCurtain(QWidget *parent, const QString &url, const QString &notes)
	: QFrame(parent) {
		setObjectName("plumeUpdateCurtain");
		setStyleSheet(u"QFrame#plumeUpdateCurtain { background: #27406b; border-radius: 12px; } QLabel { color: white; } QPushButton { background: white; color: #27406b; border: none; border-radius: 6px; padding: 8px 14px; }"_q);
		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(18, 14, 18, 14);
		auto title = new QLabel(u"Доступно обновление Плюма"_q, this);
		layout->addWidget(title);
		auto details = new QLabel(notes.isEmpty()
			? u"Новая версия готова к установке. Чаты и настройки сохранятся."_q
			: notes.left(600), this);
		details->setWordWrap(true);
		layout->addWidget(details);
		auto buttons = new QHBoxLayout();
		auto install = new QPushButton(u"Скачать и установить"_q, this);
		auto later = new QPushButton(u"Позже"_q, this);
		buttons->addWidget(install);
		buttons->addWidget(later);
		buttons->addStretch();
		layout->addLayout(buttons);
		QObject::connect(install, &QPushButton::clicked, this, [=] {
			deleteLater();
			InstallUpdate(url);
		});
		QObject::connect(later, &QPushButton::clicked, this, [=] {
			deleteLater();
		});
		parent->installEventFilter(this);
		place();
		show();
		raise();
	}
protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == parentWidget() && event->type() == QEvent::Resize) {
			place();
		}
		return QFrame::eventFilter(watched, event);
	}
private:
	void place() {
		const auto available = parentWidget()->width();
		setFixedWidth(std::max(1, std::min(560, available - 24)));
		adjustSize();
		move((available - width()) / 2, 12);
	}
};

void OfferUpdate(const QString &url, const QString &notes) {
	const auto window = Core::App().activePrimaryWindow();
	if (!window || QUrl(url).scheme() != u"https"_q) return;
	static auto curtain = QPointer<UpdateCurtain>();
	if (curtain) {
		curtain->raise();
		return;
	}
	curtain = new UpdateCurtain(window->widget()->bodyWidget(), url, notes);
}


// Фокус-режим — как LiteFocus.java на Android. tglite.json: focusMode, focusChats (id чатов строками),
// focusPrivacy<id аккаунта> — правило «Время захода» до включения, возвращается при выключении.
struct FocusState {
	bool on = false;
	base::flat_set<uint64> chats;
};

FocusState &Focus() {
	static auto result = [] {
		auto state = FocusState();
		const auto saved = ReadState();
		state.on = saved.value(u"focusMode"_q).toBool();
		for (const auto &id : saved.value(u"focusChats"_q).toArray()) {
			state.chats.emplace(id.toString().toULongLong());
		}
		return state;
	}();
	return result;
}

// «Выключить все уведомления» — одним переключателем, как LiteConfig.muteAll на Android. tglite.json: muteAll.
rpl::variable<bool> &MuteAllVariable() {
	static auto result = rpl::variable<bool>(
		ReadState().value(u"muteAll"_q).toBool());
	return result;
}

rpl::variable<bool> &FocusVariable() {
	static auto result = rpl::variable<bool>(Focus().on);
	return result;
}

// Невидимка — как LiteFocus.isGhost на Android: для всех «не в сети» и без «печатает…», чаты не прячутся.
// tglite.json: ghostMode. «Время захода» общее с фокусом (focusPrivacy<id>): «Никто», пока включён любой из двух.
rpl::variable<bool> &GhostVariable() {
	static auto result = rpl::variable<bool>(
		ReadState().value(u"ghostMode"_q).toBool());
	return result;
}

void SaveFocusChats() {
	auto list = QJsonArray();
	for (const auto id : Focus().chats) {
		list.push_back(QString::number(id));
	}
	SetState(u"focusChats"_q, list);
}

void RefreshFocusChat(not_null<Main::Session*> session, uint64 id) {
	if (const auto history = session->data().historyLoaded(PeerId(id))) {
		history->updateChatListExistence();
	}
}

QString FocusPrivacyKey(not_null<Main::Session*> session) {
	return u"focusPrivacy"_q + QString::number(session->userId().bare);
}

QJsonArray PeersToJson(const std::vector<not_null<PeerData*>> &peers) {
	auto result = QJsonArray();
	for (const auto &peer : peers) {
		result.push_back(QString::number(peer->id.value));
	}
	return result;
}

std::vector<not_null<PeerData*>> PeersFromJson(
		not_null<Main::Session*> session,
		const QJsonValue &value) {
	auto result = std::vector<not_null<PeerData*>>();
	for (const auto &id : value.toArray()) {
		const auto peerId = PeerId(id.toString().toULongLong());
		if (const auto peer = session->data().peerLoaded(peerId)) {
			result.push_back(peer);
		}
	}
	return result;
}

QJsonObject RuleToJson(const Api::UserPrivacy::Rule &rule) {
	return QJsonObject{
		{ u"option"_q, int(rule.option) },
		{ u"always"_q, PeersToJson(rule.always.peers) },
		{ u"alwaysPremiums"_q, rule.always.premiums },
		{ u"alwaysMiniapps"_q, rule.always.miniapps },
		{ u"never"_q, PeersToJson(rule.never.peers) },
		{ u"neverPremiums"_q, rule.never.premiums },
		{ u"neverMiniapps"_q, rule.never.miniapps },
	};
}

Api::UserPrivacy::Rule RuleFromJson(
		not_null<Main::Session*> session,
		const QJsonObject &json) {
	auto rule = Api::UserPrivacy::Rule();
	rule.option = Api::UserPrivacy::Option(json.value(u"option"_q).toInt());
	rule.always.peers = PeersFromJson(session, json.value(u"always"_q));
	rule.always.premiums = json.value(u"alwaysPremiums"_q).toBool();
	rule.always.miniapps = json.value(u"alwaysMiniapps"_q).toBool();
	rule.never.peers = PeersFromJson(session, json.value(u"never"_q));
	rule.never.premiums = json.value(u"neverPremiums"_q).toBool();
	rule.never.miniapps = json.value(u"neverMiniapps"_q).toBool();
	return rule;
}

void ApplyFocusPrivacy(not_null<Main::Session*> session, bool enabled) {
	using Key = Api::UserPrivacy::Key;
	const auto key = FocusPrivacyKey(session);
	auto &privacy = session->api().userPrivacy();
	if (enabled) {
		privacy.reload(Key::LastSeen);
		privacy.value(
			Key::LastSeen
		) | rpl::take(1) | rpl::on_next([=](Api::UserPrivacy::Rule rule) {
			if (!ReadState().contains(key)) {
				SetState(key, RuleToJson(rule));
			}
			auto nobody = Api::UserPrivacy::Rule();
			nobody.option = Api::UserPrivacy::Option::Nobody;
			session->api().userPrivacy().save(Key::LastSeen, nobody);
		}, session->lifetime());
	} else {
		auto state = ReadState();
		if (state.contains(key)) {
			const auto saved = state.value(key).toObject();
			state.remove(key);
			WriteState(state);
			privacy.save(Key::LastSeen, RuleFromJson(session, saved));
		}
	}
}

QString FocusChatName(not_null<Main::Session*> session, uint64 id) {
	const auto peer = session->data().peerLoaded(PeerId(id));
	return peer ? peer->name() : (u"Чат "_q + QString::number(id));
}

} // namespace

bool HideStories() {
	const auto state = ReadState();
	return !state.contains(u"hideStories"_q)
		|| state.value(u"hideStories"_q).toBool();
}

bool HideRecentSearch() {
	const auto state = ReadState();
	return !state.contains(u"hideRecentSearch"_q)
		|| state.value(u"hideRecentSearch"_q).toBool();
}

bool &HideArchiveJunkFlag() {
	static auto result = [] {
		const auto state = ReadState();
		return !state.contains(u"hideArchiveChannels"_q)
			|| state.value(u"hideArchiveChannels"_q).toBool();
	}();
	return result;
}

bool &HideArchiveBotsFlag() {
	static auto result = ReadState().value(u"hideArchiveBots"_q).toBool(true);
	return result;
}

bool &QuietGroupsFlag() {
	static auto result = ReadState().value(u"quietGroups"_q).toBool(true);
	return result;
}

bool HideArchiveJunk() {
	return HideArchiveJunkFlag() || HideArchiveBotsFlag();
}

bool HideArchiveBots() {
	return HideArchiveBotsFlag();
}

// Канал или открытая группа в архиве, где вы не создатель и не админ, или чужой бот.
// Закреплённое в архиве не прячем.
bool ArchiveJunkHidden(not_null<const History*> history) {
	if (!history->folder() || history->isPinnedDialog(FilterId())) {
		return false;
	}
	if (const auto user = history->peer->asUser()) {
		return HideArchiveBotsFlag()
			&& user->isBot()
			&& (user->id.value != kTaktBotId)
			&& (user->id.value != kMayakBotId)
			&& !(user->botInfo && user->botInfo->canEditInformation);
	}
	const auto channel = history->peer->asChannel();
	return HideArchiveJunkFlag()
		&& channel
		&& !channel->amCreator()
		&& !channel->hasAdminRights()
		&& (channel->isBroadcast() || channel->isPublic());
}

void SetHideArchiveBots(not_null<Main::Session*> session, bool enabled) {
	if (HideArchiveBotsFlag() == enabled) {
		return;
	}
	HideArchiveBotsFlag() = enabled;
	SetState(u"hideArchiveBots"_q, enabled);
	session->data().enumerateUsers([=](not_null<UserData*> user) {
		if (!user->isBot()) {
			return;
		} else if (const auto history = session->data().historyLoaded(user)) {
			history->updateChatListExistence();
		}
	});
}

bool QuietGroups() {
	return QuietGroupsFlag();
}

// Тихая группа: вы её не читаете (больше kQuietUnread непрочитанных), вы не создатель и не админ.
// Плюм не присылает от неё уведомлений, кроме упоминаний и ответов вам.
bool QuietGroup(not_null<const History*> history) {
	if (!QuietGroupsFlag() || history->unreadCount() < kQuietUnread) {
		return false;
	} else if (const auto channel = history->peer->asChannel()) {
		return channel->isMegagroup()
			&& !channel->amCreator()
			&& !channel->hasAdminRights();
	} else if (const auto chat = history->peer->asChat()) {
		return !chat->amCreator() && !chat->hasAdminRights();
	}
	return false;
}

void SetQuietGroups(bool enabled) {
	QuietGroupsFlag() = enabled;
	SetState(u"quietGroups"_q, enabled);
}

// Бот Такт спрятан: всё с ним — через Помощника, а сам бот отвечает людям на сервере.
constexpr auto kHiddenTaktBotId = uint64(8909004776); // @takt52_bot

bool &HideTaktBotFlag() {
	static auto result = ReadState().value(u"hideTaktBot"_q).toBool(true);
	return result;
}

bool HideTaktBot() {
	return HideTaktBotFlag();
}

bool TaktBotHidden(not_null<const History*> history) {
	return HideTaktBotFlag()
		&& (history->peer->id == peerFromUser(UserId(kHiddenTaktBotId)));
}

void SetHideTaktBot(not_null<Main::Session*> session, bool enabled) {
	if (HideTaktBotFlag() == enabled) {
		return;
	}
	HideTaktBotFlag() = enabled;
	SetState(u"hideTaktBot"_q, enabled);
	const auto peer = peerFromUser(UserId(kHiddenTaktBotId));
	if (const auto history = session->data().historyLoaded(peer)) {
		history->updateChatListExistence();
	}
}

bool &HideMayakBotFlag() {
	static auto result = ReadState().value(u"hideMayakBot"_q).toBool(true);
	return result;
}

bool HideMayakBot() {
	return HideMayakBotFlag();
}

bool MayakBotHidden(not_null<const History*> history) {
	return HideMayakBotFlag()
		&& (history->peer->id == peerFromUser(UserId(kMayakBotId)));
}

void SetHideMayakBot(not_null<Main::Session*> session, bool enabled) {
	if (HideMayakBotFlag() == enabled) {
		return;
	}
	HideMayakBotFlag() = enabled;
	SetState(u"hideMayakBot"_q, enabled);
	const auto peer = peerFromUser(UserId(kMayakBotId));
	if (const auto history = session->data().historyLoaded(peer)) {
		history->updateChatListExistence();
	}
}

void SetHideArchiveJunk(not_null<Main::Session*> session, bool enabled) {
	if (HideArchiveJunkFlag() == enabled) {
		return;
	}
	HideArchiveJunkFlag() = enabled;
	SetState(u"hideArchiveChannels"_q, enabled);
	const auto refresh = [=](auto peer) {
		if (const auto history = session->data().historyLoaded(peer->id)) {
			history->updateChatListExistence();
		}
	};
	session->data().enumerateGroups(refresh);
	session->data().enumerateBroadcasts(refresh);
}

bool MuteAll() {
	return MuteAllVariable().current();
}

rpl::producer<bool> MuteAllValue() {
	return MuteAllVariable().value();
}

void SetMuteAll(not_null<Window::SessionController*> controller, bool enabled) {
	if (MuteAll() == enabled) {
		return;
	}
	SetState(u"muteAll"_q, enabled);
	MuteAllVariable() = enabled;
	if (enabled) {
		Core::App().notifications().clearAll();
	}
	controller->showToast(enabled
		? u"Все уведомления выключены: Плюм молчит. Включить — там же."_q
		: u"Уведомления снова включены."_q);
}

bool FocusActive() {
	return Focus().on;
}

bool FocusHidden(not_null<const History*> history) {
	const auto &focus = Focus();
	return focus.on
		&& !focus.chats.empty()
		&& focus.chats.contains(history->peer->id.value);
}

rpl::producer<bool> FocusValue() {
	return FocusVariable().value();
}

bool GhostActive() {
	return GhostVariable().current();
}

rpl::producer<bool> GhostValue() {
	return GhostVariable().value();
}

void SetGhost(not_null<Window::SessionController*> controller, bool enabled) {
	if (GhostActive() == enabled) {
		return;
	}
	SetState(u"ghostMode"_q, enabled);
	GhostVariable() = enabled;
	const auto session = &controller->session();
	ApplyFocusPrivacy(session, enabled || FocusActive());
	session->api().updates().updateOnline();
	controller->showToast(enabled
		? u"Невидимка включена: для всех вы «не в сети», без «печатает…»."_q
		: FocusActive()
		? u"Невидимка выключена. Фокус ещё включён — вы по-прежнему «не в сети»."_q
		: u"Невидимка выключена: «Время захода» как было."_q);
}

static bool FocusBoxShown = false; // окно «Фокус-режим» открыто

void SetFocus(not_null<Window::SessionController*> controller, bool enabled) {
	auto &focus = Focus();
	if (focus.on == enabled) {
		return;
	}
	focus.on = enabled;
	SetState(u"focusMode"_q, enabled);
	FocusVariable() = enabled;
	const auto session = &controller->session();
	ApplyFocusPrivacy(session, enabled || GhostActive());
	session->api().updates().updateOnline();
	for (const auto id : focus.chats) {
		RefreshFocusChat(session, id);
	}
	controller->showToast(enabled
		? (focus.chats.empty()
			? u"Фокус включён: для всех вы не в сети. Выберите чаты, которые спрятать."_q
			: u"Фокус включён: для всех вы не в сети, выбранные чаты спрятаны."_q)
		: GhostActive()
		? u"Фокус выключен: чаты вернулись. Невидимка ещё включена."_q
		: u"Фокус выключен: «Время захода» как было, чаты вернулись."_q);
	if (enabled && focus.chats.empty() && !FocusBoxShown) {
		ShowFocusBox(controller); // без выбранных чатов прятать нечего — сразу выбор
	}
}

void ShowFocusBox(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Фокус-режим"_q));
		FocusBoxShown = true;
		box->lifetime().add([] { FocusBoxShown = false; });
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Для всех вы «не в сети» («Время захода» — «Никто», при выключении вернётся как было). "
			u"Выбранные личные чаты пропадают из списка и не присылают уведомлений, "
			u"найти их можно поиском. Включать быстро — в меню ☰ → «Фокус-режим»."_q,
			st::boxLabel));
		const auto toggle = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Фокус-режим включён"_q,
			FocusActive()));
		toggle->checkedChanges() | rpl::on_next([=](bool checked) {
			SetFocus(controller, checked);
		}, toggle->lifetime());
		FocusValue() | rpl::on_next([=](bool on) {
			toggle->setChecked(on);
		}, toggle->lifetime());

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Скрытые чаты"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto list = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			list->clear();
			if (Focus().chats.empty()) {
				list->add(object_ptr<Ui::FlatLabel>(
					list,
					u"Пока нет — добавьте ниже."_q,
					st::boxDividerLabel));
			}
			for (const auto id : Focus().chats) {
				list->add(object_ptr<Ui::LinkButton>(
					list,
					FocusChatName(session, id) + u"  ✕"_q
				))->setClickedCallback([=] {
					Focus().chats.remove(id);
					SaveFocusChats();
					RefreshFocusChat(session, id);
					(*rebuild)();
				});
			}
			list->resizeToWidth(list->width());
		};
		(*rebuild)();
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Нажмите на чат, чтобы убрать его из скрытых."_q,
			st::boxDividerLabel));
		box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"+ Добавить чат"_q))->setClickedCallback([=] {
			const auto chooser = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
			auto callback = [=](not_null<Data::Thread*> thread) {
				const auto id = thread->peer()->id.value;
				Focus().chats.emplace(id);
				SaveFocusChats();
				RefreshFocusChat(session, id);
				(*rebuild)();
				if (const auto strong = *chooser) {
					strong->closeBox();
				}
			};
			auto filter = [](not_null<Data::Thread*> thread) {
				const auto user = thread->peer()->asUser();
				return user && !user->isSelf() && !user->isBot()
					&& !Focus().chats.contains(thread->peer()->id.value);
			};
			*chooser = controller->show(Box<PeerListBox>(
				std::make_unique<ChooseRecipientBoxController>(
					session,
					std::move(callback),
					std::move(filter)),
				[](not_null<PeerListBox*> choose) {
					choose->setTitle(rpl::single(u"Какой чат спрятать"_q));
					choose->addButton(rpl::single(u"Отмена"_q), [=] {
						choose->closeBox();
					});
				}));
		});
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

QJsonObject SyncedSettings() {
	auto chats = QJsonArray();
	for (const auto id : Focus().chats) {
		if (peerIsUser(PeerId(id))) {
			chats.push_back(QString::number(id));
		}
	}
	return QJsonObject{
		{ u"hideStories"_q, HideStories() },
		{ u"hideRecentSearch"_q, HideRecentSearch() },
		{ u"hideArchiveChannels"_q, HideArchiveJunkFlag() },
		{ u"hideArchiveBots"_q, HideArchiveBotsFlag() },
		{ u"quietGroups"_q, QuietGroupsFlag() },
		{ u"muteAll"_q, MuteAll() },
		{ u"ghostMode"_q, GhostActive() },
		{ u"focusMode"_q, FocusActive() },
		{ u"focusChats"_q, chats },
		{ u"hideTaktBot"_q, HideTaktBotFlag() },
		{ u"hideMayakBot"_q, HideMayakBotFlag() },
	};
}

qint64 SettingsUpdated() {
	return qint64(ReadState().value(u"settingsUpdated"_q).toDouble());
}

void SetSettingsUpdated(qint64 updated) {
	ApplyingRemote = true;
	SetState(u"settingsUpdated"_q, double(updated));
	ApplyingRemote = false;
}

bool ApplySyncedSettings(const QJsonObject &settings, qint64 updated) {
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller) {
		return false;
	}
	const auto session = &controller->session();
	ApplyingRemote = true;
	const auto flag = [&](const QString &key, bool current, auto &&apply) {
		if (settings.contains(key) && settings.value(key).toBool() != current) {
			apply(settings.value(key).toBool());
		}
	};
	flag(u"hideStories"_q, HideStories(), [](bool v) {
		SetState(u"hideStories"_q, v);
	});
	flag(u"hideRecentSearch"_q, HideRecentSearch(), [](bool v) {
		SetState(u"hideRecentSearch"_q, v);
	});
	flag(u"hideArchiveChannels"_q, HideArchiveJunkFlag(), [&](bool v) {
		SetHideArchiveJunk(session, v);
	});
	flag(u"hideArchiveBots"_q, HideArchiveBotsFlag(), [&](bool v) {
		SetHideArchiveBots(session, v);
	});
	flag(u"hideTaktBot"_q, HideTaktBotFlag(), [&](bool v) {
		SetHideTaktBot(session, v);
	});
	flag(u"hideMayakBot"_q, HideMayakBotFlag(), [&](bool v) {
		SetHideMayakBot(session, v);
	});
	flag(u"quietGroups"_q, QuietGroupsFlag(), [](bool v) {
		SetQuietGroups(v);
	});
	flag(u"muteAll"_q, MuteAll(), [&](bool v) {
		SetMuteAll(controller, v);
	});
	if (settings.contains(u"focusChats"_q)) {
		auto chats = base::flat_set<uint64>();
		for (const auto &id : settings.value(u"focusChats"_q).toArray()) {
			if (const auto value = id.toString().toULongLong()) {
				chats.emplace(value);
			}
		}
		auto &focus = Focus();
		if (chats != focus.chats) {
			const auto was = focus.chats;
			focus.chats = chats;
			SaveFocusChats();
			for (const auto id : was) {
				RefreshFocusChat(session, id);
			}
			for (const auto id : chats) {
				RefreshFocusChat(session, id);
			}
		}
	}
	const auto shown = FocusBoxShown;
	FocusBoxShown = true; // выбор чатов с другого устройства не открываем
	flag(u"ghostMode"_q, GhostActive(), [&](bool v) {
		SetGhost(controller, v);
	});
	flag(u"focusMode"_q, FocusActive(), [&](bool v) {
		SetFocus(controller, v);
	});
	FocusBoxShown = shown;
	SetState(u"settingsUpdated"_q, double(updated));
	ApplyingRemote = false;
	return true;
}

void CheckUpdates(bool manual) {
	static auto checking = false;
	if (checking) return;
	checking = true;
	auto request = QNetworkRequest(QUrl(QString(kUpdateUrl)));
	request.setTransferTimeout(15000);
	const auto reply = Net()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		checking = false;
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
			window->showToast(u"У вас последняя версия Плюма"_q);
		}
	});
}

namespace {

base::flat_set<uint64> &HiddenAccountIds() {
	static auto result = [] {
		auto ids = base::flat_set<uint64>();
		for (const auto &v : ReadState().value(u"hiddenAccounts"_q).toArray()) {
			if (const auto id = v.toString().toULongLong()) {
				ids.emplace(id);
			}
		}
		return ids;
	}();
	return result;
}

rpl::event_stream<> &HiddenAccountsStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

QString AccountTitle(not_null<Main::Account*> account) {
	const auto user = account->session().user();
	const auto phone = user->phone();
	return phone.isEmpty()
		? user->name()
		: (user->name() + u" (+"_q + phone + u")"_q);
}

} // namespace

bool AccountHidden(not_null<Main::Account*> account) {
	const auto &ids = HiddenAccountIds();
	return !ids.empty()
		&& account->sessionExists()
		&& ids.contains(account->session().userId().bare);
}

rpl::producer<> HiddenAccountsChanges() {
	return HiddenAccountsStream().events();
}

void SetAccountHidden(not_null<Main::Account*> account, bool hidden) {
	if (!account->sessionExists() || AccountHidden(account) == hidden) {
		return;
	}
	auto &ids = HiddenAccountIds();
	const auto id = account->session().userId().bare;
	if (hidden) {
		ids.emplace(id);
	} else {
		ids.remove(id);
	}
	auto list = QJsonArray();
	for (const auto hiddenId : ids) {
		list.push_back(QString::number(hiddenId));
	}
	SetState(u"hiddenAccounts"_q, list);
	HiddenAccountsStream().fire({});
	Core::App().domain().notifyUnreadBadgeChanged();
	ReportAccounts();
}

void ShowHiddenAccountsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Скрытые аккаунты"_q));
		auto count = 0;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (!AccountHidden(account)) {
				continue;
			}
			++count;
			const auto active = (account == &controller->session().account());
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(AccountTitle(account)
						+ (active ? u" — открыт"_q : QString())),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				if (!active) {
					base::call_delayed(200, account, [=] {
						Core::App().domain().maybeActivate(account);
					});
				}
			});
		}
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			count
				? u"Нажмите — откроется этот аккаунт. Скрытые аккаунты не видны "
					u"в списке аккаунтов и не присылают уведомлений. Вернуть в список — "
					u"«Настройки Плюма» → «Аккаунты»."_q
				: u"Скрытых аккаунтов нет."_q,
			st::boxDividerLabel));
		box->addButton(rpl::single(u"Настройки Плюма"_q), [=] {
			box->closeBox();
			ShowSettingsBox(controller);
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
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
	static auto timer = new QTimer(QCoreApplication::instance());
	if (!timer->isActive()) {
		QObject::connect(timer, &QTimer::timeout, [] { CheckUpdates(false); });
		timer->start(3 * 60 * 60 * 1000);
	}
	base::call_delayed(10000, QCoreApplication::instance(), [] {
		CheckUpdates(false);
	});
}

void Stop() {
	StopXray();
}

namespace {

// Переключатель как в настройках Telegram — ползунок на всю строку.
not_null<Ui::SettingsButton*> AddToggle(
		not_null<Ui::GenericBox*> box,
		const QString &text,
		rpl::producer<bool> value) {
	const auto button = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(text),
			st::settingsButtonNoIcon),
		style::margins());
	button->toggleOn(std::move(value));
	return button;
}

not_null<Ui::SettingsButton*> AddLink(
		not_null<Ui::GenericBox*> box,
		const QString &text) {
	return box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(text),
			st::settingsButtonNoIcon),
		style::margins());
}

void AddHint(not_null<Ui::GenericBox*> box, const QString &text) {
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, text, st::boxDividerLabel),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::boxRowPadding.bottom()));
}

QFont StatFont(int size) {
	auto result = st::semiboldFont->f;
	result.setPixelSize(style::ConvertScale(size));
	return result;
}

} // namespace

void ShowSummaryBox(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	struct Noisy {
		QString name;
		int unread = 0;
	};
	auto total = 0;
	auto hidden = 0;
	auto noisy = std::vector<Noisy>();
	auto dead = std::vector<std::pair<int, not_null<ChannelData*>>>();
	const auto now = base::unixtime::now();
	const auto visit = [&](not_null<PeerData*> peer) {
		const auto history = session->data().historyLoaded(peer);
		if (!history) {
			return;
		}
		const auto hiddenHere = ArchiveJunkHidden(history);
		if (!hiddenHere && !history->inChatList()) {
			return;
		} else if (hiddenHere) {
			++hidden;
		}
		const auto unread = history->unreadCount();
		total += unread;
		if (unread > 0 && (peer->isChat() || peer->isChannel())) {
			noisy.push_back({ peer->name(), unread });
		}
		const auto channel = peer->asChannel();
		if (channel
			&& channel->amIn()
			&& !channel->amCreator()
			&& !channel->hasAdminRights()
			&& !history->isPinnedDialog(FilterId())) {
			const auto last = history->chatListTimeId();
			if (unread >= 100 || (last && now - last > 90 * 86400)) {
				dead.emplace_back(unread, channel);
			}
		}
	};
	session->data().enumerateUsers([&](not_null<UserData*> user) {
		visit(user);
	});
	session->data().enumerateGroups([&](not_null<PeerData*> peer) {
		visit(peer);
	});
	session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
		visit(channel);
	});
	ranges::sort(noisy, ranges::greater(), &Noisy::unread);
	if (noisy.size() > 5) {
		noisy.resize(5);
	}
	auto inNoisy = 0;
	for (const auto &entry : noisy) {
		inNoisy += entry.unread;
	}
	ranges::sort(dead, ranges::greater(), [](const auto &p) { return p.first; });
	if (dead.size() > 80) {
		dead.erase(dead.begin() + 80, dead.end());
	}

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Сводка"_q));
		box->setWidth(st::boxWideWidth);

		const auto tiles = box->addRow(object_ptr<Ui::RpWidget>(box));
		tiles->resize(tiles->width(), style::ConvertScale(152));
		const auto values = std::vector<std::pair<QString, QString>>{
			{ QString::number(total), u"непрочитанных"_q },
			{ (total ? QString::number(inNoisy * 100 / total) : u"0"_q) + '%',
				u"в "_q + QString::number(noisy.size()) + u" самых шумных"_q },
			{ QString::number(hidden), u"спрятано в архиве"_q },
			{ QString::number(dead.size()), u"давно не читаете"_q },
		};
		tiles->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(tiles);
			auto hq = PainterHighQualityEnabler(p);
			const auto gap = style::ConvertScale(8);
			const auto w = (tiles->width() - gap) / 2;
			const auto h = (tiles->height() - gap) / 2;
			const auto pad = style::ConvertScale(14);
			for (auto i = 0; i != int(values.size()); ++i) {
				const auto r = QRect(
					(i % 2) * (w + gap),
					(i / 2) * (h + gap),
					w,
					h);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawRoundedRect(r, pad, pad);
				p.setPen(st::windowFg);
				p.setFont(StatFont(24));
				p.drawText(
					r.x() + pad,
					r.y() + pad + style::ConvertScale(24),
					values[i].first);
				p.setPen(st::windowSubTextFg);
				p.setFont(st::normalFont);
				p.drawText(
					r.x() + pad,
					r.y() + h - pad,
					values[i].second);
			}
		}, tiles->lifetime());

		const auto section = [=](const QString &title) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(box, title, st::defaultSubsectionTitle),
				st::defaultSubsectionTitlePadding);
		};
		if (!noisy.empty()) {
			section(u"Кто больше всех шумит"_q);
			const auto rowHeight = style::ConvertScale(40);
			const auto bars = box->addRow(object_ptr<Ui::RpWidget>(box));
			bars->resize(bars->width(), rowHeight * int(noisy.size()));
			const auto max = noisy.front().unread;
			bars->paintRequest() | rpl::on_next([=] {
				auto p = QPainter(bars);
				auto hq = PainterHighQualityEnabler(p);
				const auto line = style::ConvertScale(4);
				for (auto i = 0; i != int(noisy.size()); ++i) {
					const auto top = i * rowHeight;
					const auto count = QString::number(noisy[i].unread);
					const auto countWidth = st::normalFont->width(count);
					p.setFont(st::normalFont);
					p.setPen(st::windowFg);
					p.drawText(
						QRect(0, top, bars->width() - countWidth - line * 4, rowHeight - line * 3),
						Qt::AlignLeft | Qt::AlignVCenter,
						st::normalFont->elided(
							noisy[i].name,
							bars->width() - countWidth - line * 4));
					p.setPen(st::windowSubTextFg);
					p.drawText(
						QRect(0, top, bars->width(), rowHeight - line * 3),
						Qt::AlignRight | Qt::AlignVCenter,
						count);
					const auto y = top + rowHeight - line * 2;
					p.setPen(Qt::NoPen);
					p.setBrush(st::windowBgOver);
					p.drawRoundedRect(QRect(0, y, bars->width(), line), line / 2., line / 2.);
					p.setBrush(st::activeButtonBg);
					p.drawRoundedRect(
						QRect(0, y, std::max(line, bars->width() * noisy[i].unread / std::max(max, 1)), line),
						line / 2.,
						line / 2.);
				}
			}, bars->lifetime());
		}

		struct State {
			base::flat_set<ChannelData*> chosen;
			std::vector<not_null<ChannelData*>> queue;
			int done = 0;
			bool running = false;
			rpl::variable<QString> button;
			Fn<void()> step;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto updateButton = [=] {
			state->button = state->running
				? u"Остановить"_q
				: state->chosen.empty()
				? u"Готово"_q
				: (u"Выйти из "_q + QString::number(state->chosen.size()));
		};
		if (!dead.empty()) {
			section(u"Давно не читаете"_q);
			AddHint(box, u"Каналы и группы, где больше 100 непрочитанных или 3 месяца тишины. "
				u"Свои (где вы создатель или админ) и закреплённые сюда не попадают. "
				u"Снимите галочку с того, что оставить."_q);
			for (const auto &[unread, channel] : dead) {
				state->chosen.emplace(channel.get());
				const auto check = box->addRow(object_ptr<Ui::Checkbox>(
					box,
					channel->name()
						+ (channel->isBroadcast() ? u" · канал"_q : u" · группа"_q)
						+ (unread ? (u" · "_q + QString::number(unread)) : QString()),
					true));
				const auto raw = channel.get();
				check->checkedChanges() | rpl::on_next([=](bool checked) {
					if (state->running) {
						return;
					} else if (checked) {
						state->chosen.emplace(raw);
					} else {
						state->chosen.remove(raw);
					}
					updateButton();
				}, check->lifetime());
			}
		} else {
			AddHint(box, u"Давно заброшенных каналов и групп нет — чистить нечего."_q);
		}
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		updateButton();

		state->step = [=] {
			if (!state->running) {
				return;
			} else if (state->done >= int(state->queue.size())) {
				state->running = false;
				status->setText(u"Готово: вышли из "_q
					+ QString::number(state->done)
					+ u". Публичные каналы можно вернуть поиском."_q);
				state->chosen.clear();
				updateButton();
				return;
			}
			const auto channel = state->queue[state->done++];
			session->api().leaveChannel(channel);
			status->setText(u"Выхожу: "_q
				+ QString::number(state->done)
				+ u" из "_q
				+ QString::number(state->queue.size())
				+ u" · "_q
				+ channel->name());
			base::call_delayed(3000, box, [=] { state->step(); });
		};
		box->addButton(state->button.value(), [=] {
			if (state->running) {
				state->running = false;
				status->setText(u"Остановлено: вышли из "_q
					+ QString::number(state->done)
					+ u"."_q);
				updateButton();
				return;
			} else if (state->chosen.empty()) {
				box->closeBox();
				return;
			}
			const auto count = int(state->chosen.size());
			controller->show(Ui::MakeConfirmBox({
				.text = u"Выйти из "_q
					+ QString::number(count)
					+ u" каналов и групп? По одному раз в 3 секунды, можно остановить."_q,
				.confirmed = [=](Fn<void()> close) {
					close();
					state->queue.clear();
					for (const auto &[unread, channel] : dead) {
						if (state->chosen.contains(channel.get())) {
							state->queue.push_back(channel);
						}
					}
					state->done = 0;
					state->running = true;
					updateButton();
					state->step();
				},
				.confirmText = rpl::single(u"Выйти"_q),
			}));
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Настройки Плюма"_q));
		const auto session = &controller->session();
		const auto section = [=](const QString &title) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(box, title, st::defaultSubsectionTitle),
				st::defaultSubsectionTitlePadding);
		};

		AddHint(box, u"Одни на телефоне, ПК и Mac: переключили здесь — через пару минут так же везде "
			u"(кроме VPN и аккаунтов)."_q);
		AddLink(box, u"Сводка и чистка"_q)->setClickedCallback([=] {
			ShowSummaryBox(controller);
		});

		section(u"Тишина"_q);
		const auto ghost = AddToggle(box, u"Невидимка"_q, GhostValue());
		ghost->toggledChanges() | rpl::filter([](bool on) {
			return on != GhostActive();
		}) | rpl::on_next([=](bool on) {
			SetGhost(controller, on);
		}, ghost->lifetime());
		AddHint(box, u"Для всех вы «не в сети», без «печатает…»."_q);
		const auto focus = AddToggle(box, u"Фокус"_q, FocusValue());
		focus->toggledChanges() | rpl::filter([](bool on) {
			return on != FocusActive();
		}) | rpl::on_next([=](bool on) {
			SetFocus(controller, on);
		}, focus->lifetime());
		AddLink(box, u"Чаты фокуса: "_q + QString::number(Focus().chats.size()))->setClickedCallback([=] {
			ShowFocusBox(controller);
		});
		const auto muteAll = AddToggle(box, u"Выключить все уведомления"_q, MuteAllValue());
		muteAll->toggledChanges() | rpl::filter([](bool on) {
			return on != MuteAll();
		}) | rpl::on_next([=](bool on) {
			SetMuteAll(controller, on);
		}, muteAll->lifetime());
		const auto quiet = AddToggle(box, u"Тихие группы"_q, rpl::single(QuietGroups()));
		quiet->toggledChanges() | rpl::on_next([](bool on) {
			SetQuietGroups(on);
		}, quiet->lifetime());
		AddHint(box, u"Группы, где у вас больше 30 непрочитанных, не присылают уведомлений — "
			u"кроме упоминаний и ответов вам."_q);

		section(u"Список чатов"_q);
		const auto stories = AddToggle(box, u"Скрыть истории"_q, rpl::single(HideStories()));
		stories->toggledChanges() | rpl::on_next([](bool on) {
			SetState(u"hideStories"_q, on);
		}, stories->lifetime());
		const auto recent = AddToggle(box, u"Скрыть недавние в поиске"_q, rpl::single(HideRecentSearch()));
		recent->toggledChanges() | rpl::on_next([](bool on) {
			SetState(u"hideRecentSearch"_q, on);
		}, recent->lifetime());

		section(u"Архив"_q);
		const auto junk = AddToggle(box, u"Без чужих каналов и открытых групп"_q, rpl::single(HideArchiveJunkFlag()));
		junk->toggledChanges() | rpl::on_next([=](bool on) {
			SetHideArchiveJunk(session, on);
		}, junk->lifetime());
		const auto bots = AddToggle(box, u"Без чужих ботов"_q, rpl::single(HideArchiveBotsFlag()));
		bots->toggledChanges() | rpl::on_next([=](bool on) {
			SetHideArchiveBots(session, on);
		}, bots->lifetime());
		AddHint(box, u"Свои каналы и боты, закреплённое и личные чаты остаются."_q);

		section(u"Помощник"_q);
		AddLink(box, u"Открыть помощника (Ctrl+T)"_q)->setClickedCallback([=] {
			box->closeBox();
			ShowAssistant(controller);
		});
		const auto taktOpen = AddToggle(
			box,
			u"Открывать при запуске Плюма"_q,
			rpl::single(ReadState().value(u"taktOpen"_q).toBool(true)));
		taktOpen->toggledChanges() | rpl::on_next([](bool on) {
			SetState(u"taktOpen"_q, on);
		}, taktOpen->lifetime());
		const auto hideBot = AddToggle(box, u"Спрятать бота Такт"_q, rpl::single(HideTaktBot()));
		hideBot->toggledChanges() | rpl::on_next([=](bool on) {
			SetHideTaktBot(&controller->session(), on);
		}, hideBot->lifetime());
		AddHint(box, u"Чат с @takt52_bot не виден в списке и не присылает уведомлений — "
			u"всё с ним делает Помощник. Сам бот работает и отвечает людям, как раньше."_q);

		section(u"Маяк"_q);
		AddLink(box, u"Открыть Маяк"_q)->setClickedCallback([=] {
			box->closeBox();
			OpenMayak(controller);
		});
		const auto hideMayak = AddToggle(box, u"Спрятать бота Маяка"_q, rpl::single(HideMayakBot()));
		hideMayak->toggledChanges() | rpl::on_next([=](bool on) {
			SetHideMayakBot(&controller->session(), on);
		}, hideMayak->lifetime());
		AddHint(box, u"Маяк — кнопка-маяк над чатами рядом с Помощником, вход без пароля. "
			u"«В Маяк» в меню сообщения — новый чат в Маяке с этим сообщением. Помощнику можно сказать "
			u"«поставь задачу в Маяк…» или «что в Маяке?». Спрятанный бот не виден в чатах, уведомления от него приходят."_q);

		section(u"Аккаунты"_q);
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (!account->sessionExists()) {
				continue;
			}
			const auto hide = AddToggle(
				box,
				u"Скрыть: "_q + AccountTitle(account),
				rpl::single(AccountHidden(account)));
			hide->toggledChanges() | rpl::on_next([=](bool on) {
				SetAccountHidden(account, on);
			}, hide->lifetime());
		}
		AddHint(box, u"До 30 аккаунтов: ☰ → ваше имя → «Добавить аккаунт». Скрытые не видны в списке "
			u"(кроме открытого), без уведомлений и непрочитанных; открыть — «Скрытые аккаунты» под списком. "
			u"Все аккаунты со всех устройств — plume.amdiveev.ru/accounts."_q);

		section(u"VPN StealthSurf"_q);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Плюм работает через вашу подписку, остальные программы не затрагиваются. "
			u"Ссылка — в личном кабинете StealthSurf или в Happ."_q,
			st::boxLabel));
		const auto url = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Ссылка на подписку"_q),
			VpnUrl()));
		const auto enabled = AddToggle(
			box,
			u"Плюм через VPN"_q,
			rpl::single(VpnEnabled()));
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
		section(u"Обновления"_q);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Версия "_q + QString::number(kBuild)
				+ u". Плюм сам проверяет обновления при запуске и раз в 3 часа."_q,
			st::boxDividerLabel));
		box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Проверить обновления"_q))->setClickedCallback([] {
			CheckUpdates(true);
		});
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto link = url->getLastText().trimmed();
			const auto on = enabled->toggled() && !link.isEmpty();
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
