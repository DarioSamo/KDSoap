#include "KDSoapClientInterface.h"
#include "KDSoapMessage.h"
#include "KDSoapValue.h"
#include "KDSoapPendingCallWatcher.h"
#include "KDSoapNamespaceManager.h"
#include "KDSoapServer.h"
#include "KDSoapThreadPool.h"
#include "KDSoapServerObjectInterface.h"
#include <QtTest/QtTest>
#include <QDebug>

class CountryServerObject;
typedef QMap<QThread*, CountryServerObject*> ServerObjectsMap;
ServerObjectsMap s_serverObjects;
QMutex s_serverObjectsMutex;

class PublicThread : public QThread
{
public:
    using QThread::msleep;
};

class CountryServerObject : public QObject, public KDSoapServerObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(KDSoapServerObjectInterface)
public:
    CountryServerObject() : QObject(), KDSoapServerObjectInterface() {
        //qDebug() << "Server object created in thread" << QThread::currentThread();
        QMutexLocker locker(&s_serverObjectsMutex);
        s_serverObjects.insert(QThread::currentThread(), this);
    }
    ~CountryServerObject() {
        QMutexLocker locker(&s_serverObjectsMutex);
        Q_ASSERT(s_serverObjects.value(QThread::currentThread()) == this);
        s_serverObjects.remove(QThread::currentThread());
    }

    virtual void processRequest(const KDSoapMessage &request, KDSoapMessage &response)
    {
        // Should be called in same thread as constructor
        s_serverObjectsMutex.lock();
        Q_ASSERT(s_serverObjects.value(QThread::currentThread()) == this);
        s_serverObjectsMutex.unlock();
        const QByteArray method = request.name().toLatin1();
        if (method == "getEmployeeCountry") {
            const QString employeeName = request.childValues().child(QLatin1String("employeeName")).value().toString();
            const QString ret = this->getEmployeeCountry(employeeName);
            if (!hasFault()) {
                response.setValue(QLatin1String("getEmployeeCountryResponse"));
                response.addArgument(QLatin1String("employeeCountry"), ret);
            }
        } else if (method == "getStuff") {
            const KDSoapValueList& values = request.childValues();
            const KDSoapValue valueFoo = values.child(QLatin1String("foo"));
            const KDSoapValue valueBar = values.child(QLatin1String("bar"));
            const KDSoapValue valueDateTime = values.child(QLatin1String("dateTime"));
            if (valueFoo.isNull() || valueBar.isNull() || valueDateTime.isNull()) {
                response.setFault(true);
                response.addArgument(QLatin1String("faultcode"), QLatin1String("Server.RequiredArgumentMissing"));
                return;
            }
            const int foo = valueFoo.value().toInt();
            const float bar = valueBar.value().toFloat();
            const QDateTime dateTime = valueDateTime.value().toDateTime();
            const double ret = this->getStuff(foo, bar, dateTime);
            if (!hasFault()) {
                response.setValue(ret);
            }
        } else if (method == "hexBinaryTest") {
            const KDSoapValueList& values = request.childValues();
            const QByteArray input1 = QByteArray::fromBase64(values.child(QLatin1String("a")).value().toByteArray());
            //qDebug() << "input1=" << input1;
            const QByteArray input2 = QByteArray::fromHex(values.child(QLatin1String("b")).value().toByteArray());
            //qDebug() << "input2=" << input2;
            const QByteArray hex = this->hexBinaryTest(input1, input2);
            if (!hasFault()) {
                response.setValue(QVariant(hex));
            }
        } else {
            KDSoapServerObjectInterface::processRequest(request, response);
        }
    }

public: // SOAP-accessible methods
    QString getEmployeeCountry(const QString& employeeName) {
        if (employeeName.isEmpty()) {
            setFault(QLatin1String("Client.Data"), QLatin1String("Empty employee name"),
                     QLatin1String("CountryServerObject"), tr("Employee name must not be empty"));
            return QString();
        }
        qDebug() << "getEmployeeCountry(" << employeeName << ") called";
        //if (employeeName == QLatin1String("Slow"))
        //    PublicThread::msleep(100);
        return QString::fromLatin1("France");
    }

    double getStuff(int foo, float bar, const QDateTime& dateTime) const {
        qDebug() << "getStuff called:" << foo << bar << dateTime.toTime_t();
        return double(foo) + bar + double(dateTime.toTime_t()) + double(dateTime.time().msec() / 1000.0);
    }
    QByteArray hexBinaryTest(const QByteArray& input1, const QByteArray& input2) const {
        return input1 + input2;
    }
};

class CountryServer : public KDSoapServer
{
    Q_OBJECT
public:
    CountryServer() : KDSoapServer() {}
    virtual QObject* createServerObject() { return new CountryServerObject; }

public Q_SLOTS:
    void quit() { thread()->quit(); }
};

// We need to do the listening and socket handling in a separate thread,
// so that the main thread can use synchronous calls. Note that this is
// really specific to unit tests and doesn't need to be done in a real
// KDSoap-based server.
class CountryServerThread : public QThread
{
public:
    CountryServerThread(KDSoapThreadPool* pool = 0)
        : m_threadPool(pool)
    {}
    ~CountryServerThread() {
        // helgrind says don't call quit() here (Qt bug?)
        if (m_pServer)
            QMetaObject::invokeMethod(m_pServer, "quit");
        wait();
    }
    CountryServer* startThread() {
        start();
        m_semaphore.acquire(); // wait for init to be done
        return m_pServer;
    }
protected:
    void run() {
        CountryServer server;
        if (m_threadPool)
            server.setThreadPool(m_threadPool);
        if (server.listen())
            m_pServer = &server;
        m_semaphore.release();
        exec();
        m_pServer = 0;
    }
private:
    KDSoapThreadPool* m_threadPool;
    QSemaphore m_semaphore;
    CountryServer* m_pServer;
};

class ServerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCall()
    {
        {
            CountryServerThread serverThread;
            CountryServer* server = serverThread.startThread();

            //qDebug() << "server ready, proceeding" << server->endPoint();
            KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
            const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
            QCOMPARE(response.childValues().first().value().toString(), QString::fromLatin1("France"));

            QCOMPARE(s_serverObjects.count(), 1);
            QVERIFY(s_serverObjects.value(&serverThread)); // request handled by server thread itself (no thread pool)
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

    void testParamTypes()
    {
        CountryServerThread serverThread;
        CountryServer* server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("foo"), 4);
        message.addArgument(QLatin1String("bar"), float(3.2));
        QDateTime dt = QDateTime::fromTime_t(123456);
        dt.setTime(dt.time().addMSecs(789));
        message.addArgument(QLatin1String("dateTime"), dt);
        const KDSoapMessage response = client.call(QLatin1String("getStuff"), message);
        if (response.isFault()) {
            qDebug() << response.faultAsString();
            QVERIFY(!response.isFault());
        }
        QCOMPARE(response.value().toDouble(), double(4+3.2+123456.789));
    }

    void testHexBinary()
    {
        CountryServerThread serverThread;
        CountryServer* server = serverThread.startThread();

        //qDebug() << "server ready, proceeding" << server->endPoint();
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("a"), QByteArray("KD"), KDSoapNamespaceManager::xmlSchema2001(), QString::fromLatin1("base64Binary"));
        message.addArgument(QLatin1String("b"), QByteArray("Soap"), KDSoapNamespaceManager::xmlSchema2001(), QString::fromLatin1("hexBinary"));
        const KDSoapMessage response = client.call(QLatin1String("hexBinaryTest"), message);
        QCOMPARE(QString::fromLatin1(QByteArray::fromBase64(response.value().toByteArray()).constData()), QString::fromLatin1("KDSoap"));
    }

    void testMethodNotFound()
    {
        CountryServerThread serverThread;
        CountryServer* server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        const KDSoapMessage response = client.call(QLatin1String("doesNotExist"), message);
        QVERIFY(response.isFault());
        QCOMPARE(response.arguments().child(QLatin1String("faultcode")).value().toString(), QString::fromLatin1("Server.MethodNotFound"));
        QCOMPARE(response.arguments().child(QLatin1String("faultstring")).value().toString(), QString::fromLatin1("doesNotExist not found"));
    }

    void testMissingParams()
    {
        CountryServerThread serverThread;
        CountryServer* server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("foo"), 4);
        const KDSoapMessage response = client.call(QLatin1String("getStuff"), message);
        QVERIFY(response.isFault());
        qDebug() << response.faultAsString();
    }

    void testThreadPoolBasic()
    {
        {
            KDSoapThreadPool threadPool;
            CountryServerThread serverThread(&threadPool);
            CountryServer* server = serverThread.startThread();

            KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
            const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
            QCOMPARE(response.childValues().first().value().toString(), QString::fromLatin1("France"));
            QCOMPARE(s_serverObjects.count(), 1);
            QThread* thread = s_serverObjects.begin().key();
            QVERIFY(thread != qApp->thread());
            QVERIFY(thread != &serverThread);
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

    void testMultipleThreads_data()
    {
        QTest::addColumn<int>("maxThreads");
        QTest::addColumn<int>("numRequests");
        QTest::addColumn<int>("expectedServerObjects");

        // QNetworkAccessManager only does 6 concurrent http requests
        // (QHttpNetworkConnectionPrivate::defaultChannelCount = 6)
        // so with numRequests > 6, don't expect more than 6 threads being used; for this
        // we would need more than one QNAM, i.e. more than one KDSoapClientInterface.

        QTest::newRow("5 parallel requests") << 5 << 5 << 5;
        QTest::newRow("5 requests in 3 threads") << 3 << 5 << 3;
    }

    void testMultipleThreads()
    {
        QFETCH(int, maxThreads);
        QFETCH(int, numRequests);
        QFETCH(int, expectedServerObjects);
        {
            KDSoapThreadPool threadPool;
            threadPool.setMaxThreadCount(maxThreads);
            CountryServerThread serverThread(&threadPool);
            CountryServer* server = serverThread.startThread();
            KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
            KDSoapMessage message;
            message.addArgument(QLatin1String("employeeName"), QString::fromLatin1("Slow"));
            m_returnMessages.clear();
            m_expectedMessages = numRequests;

            QList<KDSoapPendingCallWatcher*> watchers;
            for (int i = 0; i < numRequests; ++i) {
                KDSoapPendingCall pendingCall = client.asyncCall(QLatin1String("getEmployeeCountry"), message);
                KDSoapPendingCallWatcher *watcher = new KDSoapPendingCallWatcher(pendingCall, this);
                connect(watcher, SIGNAL(finished(KDSoapPendingCallWatcher*)),
                        this, SLOT(slotFinished(KDSoapPendingCallWatcher*)));
                watchers.append(watcher);
            }
            m_eventLoop.exec();

            QCOMPARE(m_returnMessages.count(), m_expectedMessages);
            Q_FOREACH(const KDSoapMessage& response, m_returnMessages) {
                QCOMPARE(response.childValues().first().value().toString(), QString::fromLatin1("France"));
            }
            QCOMPARE(s_serverObjects.count(), expectedServerObjects);
            QMapIterator<QThread*, CountryServerObject*> it(s_serverObjects);
            while (it.hasNext()) {
                QThread* thread = it.next().key();
                QVERIFY(thread != qApp->thread());
                QVERIFY(thread != &serverThread);
            }
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

    void testCallAndLogging()
    {
        // TODO
    }

    void testServerFault() // fault returned by server
    {
        CountryServerThread serverThread;
        CountryServer* server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("employeeName"), QString());
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), message);
        QVERIFY(response.isFault());
        QCOMPARE(response.arguments().child(QLatin1String("faultcode")).value().toString(), QString::fromLatin1("Client.Data"));
    }

public slots:
    void slotFinished(KDSoapPendingCallWatcher* watcher)
    {
        m_returnMessages.append(watcher->returnMessage());
        if (m_returnMessages.count() == m_expectedMessages)
            m_eventLoop.quit();
    }

private:
    QEventLoop m_eventLoop;
    int m_expectedMessages;
    QList<KDSoapMessage> m_returnMessages;

private:
    static QString countryMessageNamespace() {
        return QString::fromLatin1("http://www.kdab.com/xml/MyWsdl/");
    }
    static KDSoapMessage countryMessage() {
        KDSoapMessage message;
        message.addArgument(QLatin1String("employeeName"), QString::fromUtf8("David Ä Faure"));
        return message;
    }
};

QTEST_MAIN(ServerTest)

#include "servertest.moc"