// SimpleSMTPRelayServer.cpp

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>      // For DNS queries
#include <windows.h>     // For WideCharToMultiByte
#include <iostream>
#include <string>
#include <cstring>       // For strlen()
#include <cctype>        // For toupper()
#include <ctime>         // For time()

// Link with Ws2_32.lib and Dnsapi.lib
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Dnsapi.lib")

// Use constexpr instead of macros.
constexpr int SMTP_PORT = 25;
constexpr int BUFFER_SIZE = 1024;

using namespace std;

//---------------------------------------------------------------------
// Helper function: Convert a wide (Unicode) string to an ANSI std::string.
static string WideToString(const wchar_t* wstr) {
    if (!wstr)
        return "";
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "";
    string str(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, &str[0], len, nullptr, nullptr);
    // Remove the trailing null character if present.
    if (!str.empty() && str.back() == '\0')
        str.pop_back();
    return str;
}

//---------------------------------------------------------------------
// Helper function: Perform an MX lookup for a domain using DnsQuery_W.
// Returns the mail server with the lowest preference or an empty string on error.
static string getMxRecord(const string& domain) {
    // Convert the domain (ANSI) to a wide string.
    wstring wdomain(domain.begin(), domain.end());
    PDNS_RECORD pDnsRecord = nullptr;
    DNS_STATUS status = DnsQuery_W(wdomain.c_str(), DNS_TYPE_MX, DNS_QUERY_STANDARD, nullptr, &pDnsRecord, nullptr);
    if (status != 0) {
        cerr << "DnsQuery failed for domain " << domain << ": " << status << endl;
        return "";
    }
    string bestMx = "";
    DWORD bestPreference = 0xFFFFFFFF; // Start with a very high value.
    PDNS_RECORD pRec = pDnsRecord;
    while (pRec) {
        if (pRec->wType == DNS_TYPE_MX) {
            DWORD pref = pRec->Data.MX.wPreference;
            // pNameExchange is a wide string in the W version.
            string mxName = WideToString(pRec->Data.MX.pNameExchange);
            if (pref < bestPreference) {
                bestPreference = pref;
                bestMx = mxName;
            }
        }
        pRec = pRec->pNext;
    }
    DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
    // Remove a trailing dot, if present.
    if (!bestMx.empty() && bestMx.back() == '.')
        bestMx.pop_back();
    return bestMx;
}

//---------------------------------------------------------------------
// Helper function: Send an SMTP command over a socket and read the response.
static bool sendCommand(SOCKET sock, const string& cmd, string& response) {
    int iResult = send(sock, cmd.c_str(), static_cast<int>(cmd.length()), 0);
    if (iResult == SOCKET_ERROR) {
        cerr << "Send failed: " << WSAGetLastError() << endl;
        return false;
    }
    char buf[BUFFER_SIZE];
    int len = recv(sock, buf, BUFFER_SIZE - 1, 0);
    if (len <= 0) {
        cerr << "Recv failed: " << WSAGetLastError() << endl;
        return false;
    }
    buf[len] = '\0';
    response = string(buf);
    cout << "Remote: " << response;
    return true;
}

//---------------------------------------------------------------------
// Function: Relay the email by connecting to the recipient's MX host.
// Before sending the DATA, we check for the existence of both a Message-ID header and a From header.
// If missing, we add them.
static bool relayEmail(const string& mailFrom, const string& rcptTo, string emailData) {
    // Extract the recipient's domain from rcptTo.
    size_t atPos = rcptTo.find('@');
    if (atPos == string::npos) {
        cerr << "Invalid RCPT TO address: " << rcptTo << endl;
        return false;
    }
    size_t start = (rcptTo[0] == '<') ? 1 : 0;
    size_t end = rcptTo.find('>', atPos);
    string domain;
    if (end == string::npos)
        domain = rcptTo.substr(atPos + 1);
    else
        domain = rcptTo.substr(atPos + 1, end - atPos - 1);

    cout << "Looking up MX record for domain: " << domain << endl;
    string mxHost = getMxRecord(domain);
    if (mxHost.empty()) {
        cerr << "No MX record found for domain " << domain << endl;
        return false;
    }
    cout << "Using MX host: " << mxHost << endl;

    // Create a socket to connect to the remote mail server.
    SOCKET relaySocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (relaySocket == INVALID_SOCKET) {
        cerr << "Relay socket creation failed: " << WSAGetLastError() << endl;
        return false;
    }

    // Resolve the remote server address.
    struct addrinfo* result = nullptr, hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int iResult = getaddrinfo(mxHost.c_str(), "25", &hints, &result);
    if (iResult != 0) {
        cerr << "getaddrinfo failed: " << iResult << endl;
        closesocket(relaySocket);
        return false;
    }

    // Connect to the remote SMTP server.
    iResult = connect(relaySocket, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (iResult == SOCKET_ERROR) {
        cerr << "Connect failed: " << WSAGetLastError() << endl;
        closesocket(relaySocket);
        return false;
    }

    string response;
    char buf[BUFFER_SIZE];
    int len = recv(relaySocket, buf, BUFFER_SIZE - 1, 0);
    if (len <= 0) {
        cerr << "Relay recv failed: " << WSAGetLastError() << endl;
        closesocket(relaySocket);
        return false;
    }
    buf[len] = '\0';
    response = string(buf);
    cout << "Remote: " << response;

    // Begin a simple SMTP conversation.
    if (!sendCommand(relaySocket, "HELO localhost\r\n", response)) {
        closesocket(relaySocket);
        return false;
    }
    string cmd = "MAIL FROM:" + mailFrom + "\r\n";
    if (!sendCommand(relaySocket, cmd, response)) {
        closesocket(relaySocket);
        return false;
    }
    cmd = "RCPT TO:" + rcptTo + "\r\n";
    if (!sendCommand(relaySocket, cmd, response)) {
        closesocket(relaySocket);
        return false;
    }
    if (!sendCommand(relaySocket, "DATA\r\n", response)) {
        closesocket(relaySocket);
        return false;
    }

    // --- Insert headers if missing ---
    // If the email body is missing a "From:" header, add one.
    if (emailData.find("From:") == string::npos &&
        emailData.find("from:") == string::npos) {
        emailData = "From: " + mailFrom + "\r\n" + emailData;
    }
    // If the email body is missing a "Message-ID:" header, add one.
    if (emailData.find("Message-ID:") == string::npos &&
        emailData.find("message-id:") == string::npos) {
        time_t now = time(nullptr);
        // Generate a simple Message-ID using the current time.
        string messageId = "<" + to_string(now) + ".relay@localhost>";
        emailData = "Message-ID: " + messageId + "\r\n" + emailData;
    }
    // ------------------------------------------------

    string dataToSend = emailData + "\r\n.\r\n";
    if (!sendCommand(relaySocket, dataToSend, response)) {
        closesocket(relaySocket);
        return false;
    }
    if (!sendCommand(relaySocket, "QUIT\r\n", response)) {
        closesocket(relaySocket);
        return false;
    }

    closesocket(relaySocket);
    return true;
}

//---------------------------------------------------------------------
// Main function: Implements a simple local SMTP server.
int main() {
    WSADATA wsaData;
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    sockaddr_in serverAddr;

    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        cerr << "WSAStartup failed: " << iResult << endl;
        return 1;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        cerr << "Error at socket(): " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(SMTP_PORT);

    iResult = bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (iResult == SOCKET_ERROR) {
        cerr << "Bind failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    iResult = listen(listenSocket, SOMAXCONN);
    if (iResult == SOCKET_ERROR) {
        cerr << "Listen failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    cout << "SMTP Server is running on port " << SMTP_PORT << endl;

    clientSocket = accept(listenSocket, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET) {
        cerr << "Accept failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    const char* greeting = "220 localhost SMTP Service Ready\r\n";
    iResult = send(clientSocket, greeting, static_cast<int>(strlen(greeting)), 0);
    if (iResult == SOCKET_ERROR) {
        cerr << "Send failed: " << WSAGetLastError() << endl;
        closesocket(clientSocket);
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    bool dataMode = false; // true when receiving email content
    string mailFrom, rcptTo, emailData;
    char recvbuf[BUFFER_SIZE];
    int recvbuflen = BUFFER_SIZE;
    string commandBuffer;
    bool running = true;

    while (running) {
        iResult = recv(clientSocket, recvbuf, recvbuflen, 0);
        if (iResult > 0) {
            commandBuffer.append(recvbuf, iResult);
            size_t pos;
            while ((pos = commandBuffer.find("\r\n")) != string::npos) {
                string line = commandBuffer.substr(0, pos);
                commandBuffer.erase(0, pos + 2);

                // If in DATA mode, accumulate message body.
                if (dataMode) {
                    if (line == ".") {
                        dataMode = false;
                        cout << "\nRelaying email to remote server..." << endl;
                        if (relayEmail(mailFrom, rcptTo, emailData)) {
                            cout << "Email relayed successfully." << endl;
                            const char* response = "250 OK: Email relayed\r\n";
                            send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                        }
                        else {
                            cout << "Failed to relay email." << endl;
                            const char* response = "550 Failed to relay email\r\n";
                            send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                        }
                        // Reset the transaction state.
                        mailFrom.clear();
                        rcptTo.clear();
                        emailData.clear();
                    }
                    else {
                        emailData.append(line);
                        emailData.append("\r\n");
                    }
                    continue;
                }

                // Convert the command line to uppercase for case-insensitive comparison.
                string cmdUpper = line;
                for (auto& c : cmdUpper)
                    c = toupper(c);
                cout << "Received: " << line << endl;

                if (cmdUpper.find("HELO") == 0 || cmdUpper.find("EHLO") == 0) {
                    const char* response = "250 Hello\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
                else if (cmdUpper.find("MAIL FROM:") == 0) {
                    // Assumes "MAIL FROM:" is exactly 10 characters.
                    mailFrom = line.substr(10);
                    const char* response = "250 OK\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
                else if (cmdUpper.find("RCPT TO:") == 0) {
                    // Assumes "RCPT TO:" is exactly 8 characters.
                    rcptTo = line.substr(8);
                    const char* response = "250 OK\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
                else if (cmdUpper == "DATA") {
                    if (mailFrom.empty() || rcptTo.empty()) {
                        const char* response = "503 Bad sequence of commands\r\n";
                        send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                    }
                    else {
                        dataMode = true;
                        const char* response = "354 End data with <CR><LF>.<CR><LF>\r\n";
                        send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                    }
                }
                else if (cmdUpper == "RSET") {
                    mailFrom.clear();
                    rcptTo.clear();
                    emailData.clear();
                    dataMode = false;
                    const char* response = "250 OK\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
                else if (cmdUpper == "NOOP") {
                    const char* response = "250 OK\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
                else if (cmdUpper == "QUIT") {
                    const char* response = "221 Bye\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                    running = false;
                    break;
                }
                else {
                    const char* response = "500 Unrecognized command\r\n";
                    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
                }
            }
        }
        else if (iResult == 0) {
            cout << "Connection closing..." << endl;
            running = false;
        }
        else {
            cerr << "Recv failed: " << WSAGetLastError() << endl;
            running = false;
        }
    }

    closesocket(clientSocket);
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}
