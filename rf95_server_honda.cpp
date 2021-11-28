#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <bcm2835.h>
#include "RadioHead"
#include "RadioHead/RH_RF95.h"
#include "RadioHead/RH_RF69.h"
#include "RadioHead/RHGenericDriver.h"
#include <signal.h>
#include <sys/timeb.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <chrono>
#include <ctype.h>
#include <cstring>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <time.h>
#include <errno.h>
#include <sys/file.h>

#include <queue>
// threads
#include <thread>
#include <mutex>
#include <pthread.h>
#include <atomic>
// socket
#include <boost/asio.hpp>

using namespace boost::asio;
using ip::tcp;
using ip::address;

//screen -d -m -S shared e screen -x
#define LAST_UPDATE "20.01.2020 15:16"

// AFEletronica Multi Radio
// see <https://www.afeletronica.com.br>
//!Definicao dos pinos. ATENCAO: Nao habilite interrupcao.
#define BOARD_AF_ELETRONICA

// Now we include RasPi_Boards.h so this will expose defined
// constants with CS/IRQ/RESET/on board LED pins definition
#include "../RasPiBoards.h"

#define INIFILE "/home/pi/AFMultiRadio.ini"

#define UNIX_EPOCH 1
#define HUMAN_TIME 2


#define START_RECEIVER_MAC          18
#define FIRST_SEPARATOR             17
#define SECOND_SEPARATOR            35
#define MAC_LENGTH                  17
#define DEFAULT_PROTOCOL_LENGTH     36

/***********************************************/
#define TIME_TO_UPDATE        1000              //Time before entering to update mode.
/***********************************************/
/*! Habilita (1) ou desabilita (2) o debug. Pode ser ativado por linha de comando com
o parametro -v ou atraves do arquivo ini.*/
uint8_t DEBUG_CONSOLE = 0;

// SOCKET
/* Defines the server port */
int PORTA = 8080;
/* Server address */
std::string IP_ADDR = "192.168.0.32";/*"172.24.1.103"*/ /*"192.168.0.45"*/

//! O número de rádios que deve ser inicializado. Está parametrizado no arquivo ini, seção start, campo initialize. O padrão é 3
uint8_t number_of_radios_to_initialize = 3;

using namespace std;
//using namespace boost::posix_time;
//using namespace boost::gregorian;

static RH_RF95 _rf95;                //!Singleton Radio Head Driver


string basedir; //!diretorio primário comum aos arquivos.
string pageFile_filename;
string logdir_name; //!Diretorio e nome de arquivo de log.

uint8_t logger = 0; //!Habilita logs. Pode ser modificado atraves do arquivo ini.


static std::mutex _mutex{};
static std::queue<std::string> forwardToLoRa{};
static std::queue<std::string> forwardToServer{};


struct Connection {
    boost::asio::io_service _ioService{};
    ip::tcp::socket _socket{_ioService};
};


struct UI {
    string azul = "\033[1;34m";
    string azul_claro = "\033[36m";
    string rosa = "\033[1;7;95m";
    string verde = "\033[32m";
    string amarelo = "\033[1;33m";
    string remove_cor = "\033[0m";
};

struct ReceivedData {
    std::string rssi{};
    std::string frequency{};
    std::string instant{};
    std::string mensagem{};
    std::string remetente{};
    std::string destinatario{};
    std::string bufstr{};
    uint8_t radio;
};

/*Essas frequencias pre-definidas sao utilizadas para quando nao forem passadas as frequencias pelo arquivo
 * /home/pi/AFMultiRadio.ini ou atraves do parametro de linha de comando -f.
 * Elas sao utilizadas na carga dos valores do arquivo ini. Caso exista o arquivo ini, esses valores sao sobrescritos.*/
float frequencies[3] = {914.0, 914.25, 914.50};


/*!Mensagens de status de operacoes, sendo erro ou nao. Pode ser habilitado por linha de comando com
o parametro -v ou atraves do arquivo ini.*/
void debug(string msg);

//!Gerador de logs dos eventos de erro do sistema para auxílio na depuracao.
void log(string msg);

//!Substituicao de caracteres em um array.
char *replace_char(char *str, char find, char replace);

//!Checa se já há uma instancia rodando para não executar 2x o programa.
bool isAlreadyRunning();

/*!Converte date/time para string, utilizado no banco de dados e logs. Para que seja util, e fundamental a configuracao de servidor de hora no sistema.
 O parametro pode ser UNIX_EPOCH ou HUMAN_TIME.*/
string dateTimeToStr(uint8_t format);

//!Instancias dos rádios
RH_RF95 rf95_u2(RF_CS_PIN, RF_IRQ_PIN);
RH_RF95 rf95_u3(RF_CS_PIN2, RF_IRQ_PIN2);
RH_RF95 rf95_u7(RF_CS_PIN3, RF_IRQ_PIN3);

//!Array dos objetos do rádio, para fácil manipulacao.
RH_RF95 rf95[3] = {rf95_u2, rf95_u3, rf95_u7};

//!Utilizado no loop para chavear a leitura dos rádios.
unsigned char radioNumber = 0;

/*!Estrutura de servicos dedicados a cada rádio. Esses valores sao passados atraves do arquivo /home/pi/AFMultiRadio.ini.
 * listener: rádios que fazem escuta.
 * sender: rádios que enviam mensagem
 * bridge: rádios que fazem a transferencia dos arrays de dados das coletas de leitura para os demais concentradores.
 * starts: rádios que devem ser inicializados com o programa, para placas de 1 a 3 rádios. A ordem é u2, u3 e u7
 * Os listeners podem cumprir mais de uma tarefa em alternancia com um dos demais servicos sem que haja implicacoes na
 * integridade dos dados.*/
struct srvs {
    bool listener[3] = {false};
    bool sender[3] = {false};
    bool bridge[3] = {false};
    uint8_t number_of_radios_to_initialize = 3;
} services;

//!Flag for Ctrl-C para interromper o programa.
volatile sig_atomic_t force_exit = false;

string dateTimeToStr(uint8_t format) {
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (format == UNIX_EPOCH) {
        return to_string(rawtime);
    }

    char *logdt = asctime(timeinfo);

    char *log_datetime = replace_char(logdt, '\n', ' ');
    return log_datetime;
}

bool isAlreadyRunning() {
    int pid_file = open("/var/run/AFMultiRadio.pid", O_CREAT | O_RDWR, 0666);
    int rc = flock(pid_file, LOCK_EX | LOCK_NB);
    if (rc) {
        if (EWOULDBLOCK == errno) {
            perror("\033[1;31mOutra instancia rodando ou lock nao removido (/var/run/AFMultiRadio.pid)\033[0m\n\n");
            exit(0);
        }
    }
}

char *replace_char(char *str, char find, char replace) {
    char *current_pos = strchr(str, find);
    while (current_pos) {
        *current_pos = replace;
        current_pos = strchr(current_pos, find);
    }
    return str;
}

void log(string msg) {
    if (logger == 0) {
        return;
    }

    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    char *logdt = asctime(timeinfo);

    char *log_datetime = replace_char(logdt, '\n', ' ');

    char msglog[200] = {0};
    strcpy(msglog, "[ ");
    strcat(msglog, log_datetime);
    strcat(msglog, " ] ");
    strcat(msglog, msg.c_str());

    cout << msglog << endl;

    if (sizeof(logdir_name) < 2) {
        return;
    }

    fstream logfile;
    logfile.open(logdir_name, fstream::in | fstream::out | fstream::app);
    if (!logfile.is_open()) {
        debug("Nao foi possivel gravar log.");
        logfile.close();
        return;
    }

    logfile << msglog << "\n";
    logfile.close();

}

//!Manipulador de tempo para se assemelhar ao millis do Arduino. Utilizado em conjunto com o millis declarado seguidamente a essa funcao.
int getMillis() {
    timeb tb;
    ftime(&tb);
    int nCount = tb.millitm + (tb.time & 0xfffff) * 1000;
    return nCount;
}

//!Manipulador de tempo para se assemelhar ao millis do Arduino
int millis(int val) {
    int nSpan = getMillis() - val;
    if (nSpan < 0)
        nSpan += 0x100000 * 1000;
    return nSpan;
}

void debug(uint8_t *msg) {
    if (DEBUG_CONSOLE == 1) {
        cout << msg;
    }
}


void debug(string msg) {
    if (DEBUG_CONSOLE == 1) {
        cout << msg;
    }
}

//!Manipulador da interrupcao com Ctrl+C
void sig_handler(int sig) {
    printf("\n%s Break received, exiting!\n", __BASEFILE__);
    log("Programa encerrado");
    //FECHA A BASE DE DADOS SE FOI SOLICITADO O ENCERRAMENTO DO PROGRAMA

    force_exit = true;
}

void splitBuf(char *buf, ReceivedData &data) {
    data.remetente = std::string(std::strtok(buf, "!"));
    data.destinatario = std::string(std::strtok(nullptr, "!"));

    std::string project = std::string(std::strtok(nullptr, "!"));

    data.mensagem = std::string(std::strtok(nullptr, ""));

    data.bufstr = data.remetente + '!' + data.destinatario + '!' + project + '!' + data.mensagem;
}


/**
 * printa a chegada da mensagem e adiciona ela na
 * fila de envio para o servidor (forwardToServer)
 * @param ui estrutura com cores para estilizar output
 * @param from número que representa o id do LoRa que enviou
 * @param radio número do rádio que recebeu
 * @param data estrutura que abstrai os dados da mensagem que chegou
 */
void receiveMessage(UI ui, uint8_t from, uint8_t radio, ReceivedData &data) {

    debug("[ " + ui.amarelo + dateTimeToStr(HUMAN_TIME) + ui.remove_cor + "] ");
    debug("(" + ui.azul_claro + "radio ");
    debug(to_string(radio));
    debug(" " + to_string(frequencies[radio]) + "Mhz");
    debug(ui.remove_cor + "): ");

    debug("addr: ");
    debug(to_string(from) + " ");
    debug("buffer: ");
    debug(ui.verde + data.bufstr + ui.remove_cor);
    debug("\n");

    // insere a mensagem na fila
    forwardToServer.push(data.bufstr);

//    std::cout << ui.azul_claro;
//    std::cout << "\t\tMensagem: " << data.bufstr << " adicionada na fila\n";
//    std::cout << "\t\tTamanho atual: " << forwardToServer.size() << "\n";
//    std::cout << ui.remove_cor;
}

bool isValidMessage(uint8_t *buf) {
    const uint8_t mac[] = "b8:27:eb:8e:94:f2";
    uint8_t separador = 33;     // !
    if (buf[FIRST_SEPARATOR] == separador && buf[SECOND_SEPARATOR] == separador) {
        for (int i = 0, j = START_RECEIVER_MAC;
             i < MAC_LENGTH, j < SECOND_SEPARATOR; i++, j++) {
            if (mac[i] != buf[j]) {
                return false;
            }
        }
    } else {
        return false;
    }
    return true;
}

/**
 * Caso a flag de debug esteja ativa printa na tela que a mensagem foi ignorada pelo gateway
 *
 * @param ui estrutura com cores para estilizar output
 * @param from número que representa o id do LoRa que enviou
 * @param radioNumber número do rádio que recebeu
 * @param buf mensagem que chegou
 */
void ignoreMessage(UI ui, uint8_t from, uint8_t radioNumber, uint8_t *buf) {
    debug(ui.rosa);
    debug(" ::: IGNORANDO MENSAGEM A SEGUIR ::: \n");
    debug(ui.rosa + "Mensagem invalida - rádio ");
    debug(to_string(radioNumber));
    debug(" Addr: ");
    debug(to_string(from));
    debug(" Buffer: ");
    debug(buf);
    debug("\n");
    debug(ui.remove_cor);
}

/**
 * Executa a leitura do socket e remove o delimitador estabelecido no protocolo '\n'
 * @param socket conexão com o servidor
 * @return mensagem lida do socket
 */
std::string readSocket(tcp::socket &socket) {
    boost::asio::streambuf buf;
    read_until(socket, buf, "\n");
    std::string data = buffer_cast<const char *>(buf.data());
    data.pop_back();    // remove '\n'
    return data;
}

/**
 * Executa escrita no socket
 * @param socket conexão com servidor
 * @param message mensagem para ser enviado
 */
void writeOnSocket(tcp::socket &socket, const std::string &message) {
    write(socket, buffer(message + "\n"));
}

/**
 * Exibe a mensagem recebida na tela, só é utilizado em caso de debug com o código no gateway.
 * A flag deve ser trocada manualmente. Ainda, esse método só deve ser utilizado nas comunicações com o servidor.
 * @param msg mensagem que será exibida
 */
void debugSocket(std::string msg) {
    bool flag = false;
    if (flag) {
        std::cout << msg;
    }
}


/**
 * Loop infinito responsável por fazer leitura do socket. Para cada leitura executada
 * envia uma confirmação 'OK' para o servidor para liberar a escrita/leitura
 * @param connection possui a conexão socket
 */
void reader(Connection *connection) {
    UI ui{};
    while (!force_exit) {
        _mutex.lock();
        if (connection->_socket.available() > 0) {
            debugSocket("\tIniciando leitura do socket\n");

            std::string data = readSocket(connection->_socket);

            debugSocket("\tChegou a mensagem: " + data + "\n");

            const std::string OK = "OK";

            debugSocket("\tConfirmação de chegada enviada 'OK'\n");

            writeOnSocket(connection->_socket, OK);

            debugSocket("\tEnvio da mensagem executado\n");
            debugSocket("\tMensagem adicionada na fila\n");

            forwardToLoRa.push(data);
        }
        _mutex.unlock();
    }
}

/**
 * Loop infinito responsável por executar escrita no socket. Para cada escrita executada
 * fica a espera de uma resposta no formato de 'OK' do servidor para liberar a escrita/leitura
 * @param connection possui a conexão socket
 */
void writer(Connection *connection) {
    while (!force_exit) {
        _mutex.lock();
        if (!forwardToServer.empty()) {
            const std::string message = forwardToServer.front();
            forwardToServer.pop();
            debugSocket("\tMensagem retirada da fila para envio: " + message + "\n");
            debugSocket("\tIniciando escrita no socket\n");

            writeOnSocket(connection->_socket, message);

            debugSocket("\tEnvio executado, esperando resposta\n");

            auto response = readSocket(connection->_socket);

            debugSocket("\tA resposta para \'" + message + "\' chegou -> " + response + "\n");
        }
        _mutex.unlock();
    }
}

/**
 * Se a fila de envio (forwardToLoRa) possuir mensagens executa um loop
 * para enviar todas as mensagens até que não sobre nenhuma
 */
void deliverToLoRa() {
    UI ui{};
    // ativa o lock para que as outras threads não interfiram na execução
    _mutex.lock();
    if (!forwardToLoRa.empty()) {
        debug(":::Iniciando entrega de mensagens:::\n");
        debug("[ " + ui.amarelo + dateTimeToStr(HUMAN_TIME) + ui.remove_cor + "] ");
        debug("Transmitindo na frequencia: " + to_string(frequencies[2]));
        debug("\n");
    }
    while (!forwardToLoRa.empty()) {
        // Recupera o primeiro elemento da fila, e retira ele logo em seguida
        std::string str = forwardToLoRa.front();
        forwardToLoRa.pop();

        // cria um vetor de uint8_t (unsigned char)
        uint8_t data[str.size()];
        // preenche a ultima posição com o terminador de string '\0'
        memset(data, '\0', sizeof(data) + 1);
        // transfere os caracteres da mensagem que estava na fila para o vetor uint8_t
        for (size_t i = 0; i < str.size(); i++) data[i] = str.at(i);

        debug("Enviando mensagem " + ui.verde + "[");
        debug(data);
        debug("] ");
        debug(to_string(str.size()) + "Bytes" + ui.remove_cor);
        debug("\n");
        // envia a mensagem
        rf95[2].send(data, sizeof(data));
        rf95[2].waitPacketSent();

    }
    // desativa o lock
    _mutex.unlock();
}


/*! Funcao principal. Todo o programa e executado dentro dessa funcao. A primeira tarefa e validar o usuário; se
nao for root, sai imediatamente. Executar como root e mandatorio devido a acessos privilegiados do BCM.*/
int main(int argc, char *argv[]) {
    isAlreadyRunning();

    cout << string(100, '\n');

    /*O BCM faz acesso exclusivos que nao podem ser manipulados pelo usuário, portanto e necessário verificar quem o está executando para evitar
     * mensagens de erros que parecam-se com bug. O programa nao executa como usuário mesmo sem essa validacao.*/
    if (getuid()) {
        cout << "O programa deve ser rodado como root. Saindo..." << endl;
        exit(0);
    }

    //!Utilizando boost para a leitura de arquivo ini com as predefinicoes de inicializacao.
    if (FILE *f = fopen(INIFILE, "r")) {
        fclose(f);
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(INIFILE, pt);

        /*Se nao houver definicao do arquivo ini, utiliza os proprios valores definidos no array, no início das declaracoes do codigo.*/
        frequencies[0] = pt.get<float>("frequency.radio0",
                                       frequencies[0]); //!Frequencia do rádio 0 configurado no arquivo /home/pi/AFMultiRadio.ini
        frequencies[1] = pt.get<float>("frequency.radio1",
                                       frequencies[1]); //!Frequencia do rádio 1 configurado no arquivo /home/pi/AFMultiRadio.ini
        frequencies[2] = pt.get<float>("frequency.radio2",
                                       frequencies[2]); //!Frequencia do rádio 2 configurado no arquivo /home/pi/AFMultiRadio.ini

        logdir_name = pt.get<string>("debug.logsdir",
                                     "/dev/shm/AFMultiRadio.log"); //!Seleciona diretorio de logs, configurado no arquivo /home/pi/AFMultiRadio.ini
        logger = pt.get<uint8_t>("debug.logger",
                                 1); //!Ativa (ou nao) o log, configurado no arquivo /home/pi/AFMultiRadio.ini
        DEBUG_CONSOLE = pt.get<uint8_t>("debug.verbose",
                                        0); //!Exibe mensagem de debug em modo verboso, configurado no arquivo /home/pi/AFMultiRadio.ini

        services.listener[0] = pt.get<bool>("listener.radio0",
                                            true); //!Escuta por clients no rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.listener[1] = pt.get<bool>("listener.radio1",
                                            true); //!Escuta por clients no rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.listener[2] = pt.get<bool>("listener.radio2",
                                            true); //!Escuta por clients no rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)

        services.sender[0] = pt.get<bool>("sender.radio0",
                                          true); //!Envio de firmware pelo rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.sender[1] = pt.get<bool>("sender.radio1",
                                          false); //!Envio de firmware pelo rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.sender[2] = pt.get<bool>("sender.radio2",
                                          false); //!Envio de firmware pelo rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)

        services.bridge[0] = pt.get<bool>("bridge.radio0",
                                          false); //!Transferencia entre concentradores, pelo rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.bridge[1] = pt.get<bool>("bridge.radio1",
                                          false); //!Transferencia entre concentradores, pelo rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.bridge[2] = pt.get<bool>("bridge.radio2",
                                          true); //!Transferencia entre concentradores, pelo rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)

        number_of_radios_to_initialize = pt.get<uint8_t>("start.initialize",
                                                         3); //Define o número de rádios que tem a placa. Os rádios serão inicializados na ordem u2, u3 e u7. Na ausência do parâmetro, os 3.

        string dname = pt.get<string>("database.dbname");


        IP_ADDR = pt.get<string>("socket.ip", "192.168.0.32");
        PORTA = pt.get<int>("socket.porta", 8080);

    }

    //!Logar a inicializacao do programa e importante para definir uma timeline em uma cadeia de eventos.
    log("Programa inicializado");

    //parsing argument flags
    int vflag = 0; //!Flag do modo verboso, chamado com -v por linha de comando ou atraves do arquivo ini.
    int dflag = 0; //!Selecao de base de dados alternativa para fins de debug, chamado com o parametro -d seguido do caminho+nome.
    int fflag = 0; //!Ajuste de frequencias com -f frq1,freq2,freq3. Exemplo em -h (help). Valores padrao definidos internamente e tambem por arquivo ini.
    int hflag = 0; //!Flag de ajuda, chamado com -h por linha de comando para exibir as opcoes e exibir exemplo.

    char *dvalue = NULL; //!Armazena o argumento para o parametro -d (base de dados).
    char *fvalue = NULL; //!Armazena o argumento para o parametro -f (frequencias dos rádios).

    int index;
    int c;
    opterr = 0;

    float t;
    int pos = 0;

    char *fq = NULL;
    while ((c = getopt(argc, argv, "vdfh")) != -1)

        switch (c) {
            case 'h':
                hflag = 1;
                cout << endl;
                cout << "Usage: ";
                cout << argv[0] << endl;
                cout << "Params:" << endl;
                cout << "-v    Verbose execution" << endl;
                cout << "-d    Select database name (default:/dev/shm/AFMultiRadio.sqlite3)"
                     << endl;
                cout << "-f    Frequencies. Ex: 914.75,914.125,914.0" << endl;
                exit(0);

            case 'v':
                vflag = 1;
                DEBUG_CONSOLE = 1;
                break;
            case 'f':
                fvalue = (char *) argv[optind];
                fq = strtok(fvalue, ",");

                while (fq != NULL) {
                    //printf("::: %s :::\n",fq);

                    t = atof(fq);
                    frequencies[pos] = t;
                    fq = strtok(NULL, ",");
                    pos++;
                }

            case '?':
                if (optopt == 'r' || optopt == 'd' || optopt == 'f' || optopt == 'u') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                } else if (isprint(optopt)) {
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                } else if (optopt == '\x0') {
                    uint8_t x = 0; //so pra evitar esse bug ate descobrir a razao
                } else {
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                }
                //return 1;
            default:
                //abort();
                int x = 1;
        }


    cout << "\033[1;33mUltima atualizacao: " << LAST_UPDATE << "\033[0m" << endl << endl << endl;

    struct stat st;

    unsigned long _lastMessageMillis = 0;

    unsigned long led_blink = 0;

    signal(SIGINT, sig_handler);
    //printf( "%s\n", __BASEFILE__);

    if (!bcm2835_init()) {
        fprintf(stderr, "%s bcm2835_init() Failed\n\n", __BASEFILE__);
        return 1;
    }

#ifdef RF_RST_PIN
    //printf( ", RST=GPIO%d", RF_RST_PIN );
    // Pulse a reset on module
    pinMode(RF_RST_PIN, OUTPUT);
    digitalWrite(RF_RST_PIN, LOW );
    bcm2835_delay(150);
    digitalWrite(RF_RST_PIN, HIGH );
    bcm2835_delay(100);
#endif

    for (int i = 0; i < number_of_radios_to_initialize; i++) {
        if (!rf95[i].init()) {
            string m = "(radio " + to_string(i) + " ERROR!!!)";
            debug(m + "\n");
            log(m);
            fprintf(stderr,
                    "\033[1;31m::: RF95 module init failed. Please, check the board :::\033[0m\n\n");
        } else {
            string msg = "\033[32mRadio " + to_string(i) + " started. Setting up...\033[0m\n";
            debug(msg);
        }
    }


    // Adjust Frequency
    for (int i = 0; i < number_of_radios_to_initialize; i++) {
        debug("Settings of radio " + to_string(i) + "\n");

        rf95[i].setTxPower(13, false);

        debug("Setting up band (" + to_string(frequencies[i]) + "MHz)\n");
        rf95[i].setFrequency(frequencies[i]);
        rf95[i].setPromiscuous(true);
        rf95[i].setModeRx();
    }

    debug("\n\nWaiting message!\n\n");

    UI ui{};

    // struct com a comunicação socket encapsulada
    Connection connection{};
    // gera uma conexão socket com o IP e a porta inseridas no arquivo .ini
    connection._socket.connect(tcp::endpoint(
            address::from_string(IP_ADDR),
            PORTA)
    );

    // Thread responsável por ler o socket
    std::thread readerWorker(reader, &connection);
    // Thread responsável por escrever no socket
    std::thread writerWorker(writer, &connection);


    //SE PRECISAR FAZER DEBUG APENAS NO PRIMEIRO RADIO,
    //BASTA COMENTAR O OPERADOR TERNARIO NO FINAL DO LOOP
    while (!force_exit) {

        if (rf95[radioNumber].available()) {
            // Should be a message for us now
            uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
            uint8_t len = sizeof(buf);
            uint8_t from = rf95[radioNumber].headerFrom();

            _lastMessageMillis = getMillis();

            if (rf95[radioNumber].recv(buf, &len)) {
                if (isValidMessage(buf)) {
                    // Cria um struct que contém todos os dados da mensagem que chegou pelo LoRa
                    ReceivedData data{};
                    // Seta valores que poderão ser usados futuramente
                    data.rssi = to_string(rf95[radioNumber].lastRssi());

                    data.frequency = to_string(frequencies[radioNumber]);
                    data.radio = radioNumber;
                    // Executa split da mensagem
                    splitBuf((char *) buf, data);
                    // Executa o protocolo de recebimento da mensagem LoRa
                    receiveMessage(ui, from, radioNumber, data);
                } else {
                    ignoreMessage(ui, from, radioNumber, buf);
                }
//                log("Sender -> " + to_string(forwardToLoRa.size()) + " | Recv ->" + to_string(forwardToServer.size()));
            } // if recv
            else {
                debug("Erro ao receber mensagem :(\n");
                log("Erro na recepcao de mensagem");

            }
        } // if radio available
        else if ((millis(_lastMessageMillis)) > TIME_TO_UPDATE && radioNumber == 2) {
            _lastMessageMillis = getMillis();
            deliverToLoRa();
        }

        // Tempo para execucao de outras tarefas
        // Pode ser aumentado ou reduzido, mas reduzir esse delay pode sobrecarregar a CPU
        // Recomendado nao mexer
        bcm2835_delay(5);

        //COMENTE ESSA LINHA PARA USAR APENAS O RADIO 0
        radioNumber = radioNumber > number_of_radios_to_initialize - 2 ? 0 : radioNumber + 1;

    }//while (!force_exit)

    connection._socket.close();


    printf("\n%s Ending\n", __BASEFILE__);
    bcm2835_close();
    return 0;
}
