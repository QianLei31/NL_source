#pragma once

#include <QObject>
#include <QString>

namespace ccv2 {

class NetworkState : public QObject {
    Q_OBJECT

public:
    explicit NetworkState(const QString &host, int port, int dataPort = 5001, QObject *parent = nullptr)
        : QObject(parent),
          m_host(host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed()),
          m_port(port),
          m_dataPort(dataPort) {}

    QString host() const { return m_host; }
    int port() const { return m_port; }
    int dataPort() const { return m_dataPort; }

    void setEndpoint(const QString &host, int port) {
        setEndpoint(host, port, m_dataPort);
    }

    void setEndpoint(const QString &host, int port, int dataPort) {
        const QString normalizedHost = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
        const int normalizedPort = port;
        const int normalizedDataPort = dataPort;
        if (normalizedHost == m_host && normalizedPort == m_port && normalizedDataPort == m_dataPort) {
            return;
        }
        const bool endpointChangedValue = normalizedHost != m_host || normalizedPort != m_port;
        const bool dataPortChangedValue = normalizedDataPort != m_dataPort;
        m_host = normalizedHost;
        m_port = normalizedPort;
        m_dataPort = normalizedDataPort;
        if (endpointChangedValue) {
            emit endpointChanged(m_host, m_port);
        }
        if (dataPortChangedValue) {
            emit dataPortChanged(m_dataPort);
        }
        emit networkChanged(m_host, m_port, m_dataPort);
    }

signals:
    void endpointChanged(const QString &host, int port);
    void dataPortChanged(int dataPort);
    void networkChanged(const QString &host, int port, int dataPort);

private:
    QString m_host;
    int m_port{10086};
    int m_dataPort{5001};
};

}  // namespace ccv2
