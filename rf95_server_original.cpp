#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <bcm2835.h>
#include "RH_RF95.h"
#include "RH_RF69.h"
#include <signal.h>
#include <sys/timeb.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <sqlite3.h>
#include <chrono>
#include <ctype.h>
#include <cstring>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <time.h>
#include <errno.h>
#include <sys/file.h>
#include <sio_client.h>

/* SUPORTE
 * Para requerer servico de suporte e/ou implementacao, entre em contato com a AFEletronica <afeletronicavendas@gmail.com> ou Do bit Ao Byte <djames.suhanko@gmail.com>.
 * Todo e qualquer suporte, duvidas, consultoria, modelagem ou implementacao serao tratados como servico, que teremos o prazer de prestar, mediante negociacao previa
 * e levantamento de requisitos.
 * 
 * Dica antecipada:
 * - Leia a documentacao enviada pela AFEletronica
 * - o client devera mandar uma mensagem no formato $abcd\n, criado como exemplo no seguinte formato:
 *   (BUF_END = 5): (0) $ (1) counter (2) counter (3) bat (4) violation (5) \n)
 * - O codigo deve ser adaptado para sua necessidade, ou simplesmente utilizado como base para suas implementacoes.
 * */


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


//exemplo de mensagem, comećando em $ e terminando com \n. Os Bytes internos representam a mensagem
//!Primeiro byte do buffer
#define BUF_START 0
//Ultimo byte do buffer
#define BUF_END   5

/***********************************************/
#define MESSAGE_LENGTH		6				//6 Bytes used in the message ((0)$ (1)sensorA (2)sensorB (3)sensorC (4)sensorD (5)\n)
#define TIME_TO_UPDATE		1000			//Time before entering to update mode.
/***********************************************/
/*! Habilita (1) ou desabilita (2) o debug. Pode ser ativado por linha de comando com
o parametro -v ou atraves do arquivo ini.*/
uint8_t DEBUG_CONSOLE = 0;

sio::client h;


//! O número de rádios que deve ser inicializado. Está parametrizado no arquivo ini, seção start, campo initialize. O padrão é 3
uint8_t number_of_radios_to_initialize = 3;

using namespace std;
//using namespace boost::posix_time;
//using namespace boost::gregorian;

/***********************************************/
//!TYPES - enumeradores para status
/***********************************************/
enum LoraServerState
{
	RADIO_INIT,
	RUNNING,
	ERROR,
	INIT_CONSOLE
};

//Exemplo de formato de mensagem
enum DecoderState
{
	START,
	COUNTER,
	BATTERY,
	END
};

static RH_RF95 _rf95;				//!Singleton Radio Head Driver
static LoraServerState _state;		//!State Machine


string basedir; //!diretorio primário comum aos arquivos.
string pageFile_filename;
string logdir_name; //!Diretorio e nome de arquivo de log.

uint8_t logger = 0; //!Habilita logs. Pode ser modificado atraves do arquivo ini.

//map <uint8_t, long int> clients;
uint8_t clients[256][5];

//!Variável utilizada para compor as queries do banco de dados.
char q[150] = {0};

struct dbase{
    uint8_t max_db_size    = 40;
    char hs_database[150]  = {0};
    bool using_db          = true;
    string result          = " ";
    int query_status       = 0;
    int rc                 = 254;
    sqlite3 *db;

} dbConf;

/*Essas frequencias pre-definidas sao utilizadas para quando nao forem passadas as frequencias pelo arquivo
 * /home/pi/AFMultiRadio.ini ou atraves do parametro de linha de comando -f.
 * Elas sao utilizadas na carga dos valores do arquivo ini. Caso exista o arquivo ini, esses valores sao sobrescritos.*/
float frequencies[3] = {914.0,914.25,914.50};

//! Separa os bytes da mensagem recebida em suas respectivas variáveis
void DecodeAndSendToRest(const uint8_t* message, const uint8_t ui8BoardId);

/*!Mensagens de status de operacoes, sendo erro ou nao. Pode ser habilitado por linha de comando com
o parametro -v ou atraves do arquivo ini.*/
void debug(string msg);

void clearClients();

//!Callback da operacao em base de dados.
static int callback(void *NotUsed, int argc, char **argv, char **azColName);

//!Executor das queries do banco de dados. Utilizado para inserir os clients e seus valores, como forma de backup.
void db_query(const char *query, bool usedb);

//!Gerador de logs dos eventos de erro do sistema para auxílio na depuracao.
void log(string msg);

//!Substituicao de caracteres em um array.
char* replace_char(char* str, char find, char replace);

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
RH_RF95 rf95[3] = {rf95_u2,rf95_u3,rf95_u7};

//!Utilizado no loop para chavear a leitura dos rádios.
unsigned char radioNumber = 0;

/*!Estrutura de servicos dedicados a cada rádio. Esses valores sao passados atraves do arquivo /home/pi/AFMultiRadio.ini.
 * listener: rádios que fazem escuta.
 * sender: rádios que enviam mensagem
 * bridge: rádios que fazem a transferencia dos arrays de dados das coletas de leitura para os demais concentradores.
 * starts: rádios que devem ser inicializados com o programa, para placas de 1 a 3 rádios. A ordem é u2, u3 e u7
 * Os listeners podem cumprir mais de uma tarefa em alternancia com um dos demais servicos sem que haja implicacoes na
 * integridade dos dados.*/
struct srvs{
    bool listener[3]                         = {false};
    bool sender[3]                           = {false};
    bool bridge[3]                           = {false};
    uint8_t number_of_radios_to_initialize   = 3;
} services;

//!Flag for Ctrl-C para interromper o programa.
volatile sig_atomic_t force_exit = false;

string dateTimeToStr(uint8_t format){
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    if (format == UNIX_EPOCH){
        return to_string(rawtime);
    }

    char * logdt = asctime(timeinfo);

    char *log_datetime = replace_char(logdt,'\n',' ');
    return log_datetime;
}

bool isAlreadyRunning(){
    int pid_file = open("/var/run/AFMultiRadio.pid", O_CREAT | O_RDWR, 0666);
    int rc       = flock(pid_file, LOCK_EX | LOCK_NB);
    if (rc){
        if (EWOULDBLOCK == errno){
            perror("\033[1;31mOutra instancia rodando ou lock nao removido (/var/run/AFMultiRadio.pid)\033[0m\n\n");
            exit(0);
        }
    }
}

char* replace_char(char* str, char find, char replace){
    char *current_pos = strchr(str,find);
    while (current_pos){
        *current_pos = replace;
        current_pos = strchr(current_pos,find);
    }
    return str;
}

void log(string msg){
    if (logger == 0){
        return;
    }

    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    char * logdt = asctime(timeinfo);

    char *log_datetime = replace_char(logdt,'\n',' ');

    char msglog[200] = {0};
    strcpy(msglog,"[ ");
    strcat(msglog,log_datetime);
    strcat(msglog," ] ");
    strcat(msglog,msg.c_str());

    cout << msglog << endl;

    if (sizeof(logdir_name) < 2){
        return;
    }

    fstream logfile;
    logfile.open(logdir_name, fstream::in | fstream::out | fstream::app);
    if (!logfile.is_open()){
        debug("Nao foi possivel gravar log.");
        logfile.close();
        return;
    }

    logfile << msglog << "\n";
    logfile.close();

}
//TODO: Fazer o processo de copia periodica da base de dados para manipulacao a posteriori.
void db_query(const char *query, bool usedb){
    if (!usedb){
        return;
    }
    char *zErrMsg = 0;
    //Na declaracao na estrutura dbConf está > 0 (nao OK), entao tenta abrir se for a primeira vez...
    if (dbConf.rc){ //SQLITE_OK == 0 para ok
        //MAS se nao conseguir abrir, dá mensagem de erro e tenta fechar mesmo assim.
        dbConf.rc = sqlite3_open(dbConf.hs_database, &dbConf.db);
        if (dbConf.rc){
            string msg = "Couldn't open database \n";
            log(msg);
            //memset(q,0,dbConf.max_query_size);
            log("Nao foi possivel executar a query");
            log(query);
            log("Base de dados:");
            log(dbConf.hs_database);
            //log("Base de dados desabilitada agora.");
            debug(msg);

            sqlite3_close(dbConf.db);
            //dbConf.using_db = false;
            dbConf.query_status = -1;
            return;
       }
    }
    //SE JA ESTIVER ABERTA, IGNORA AS DUAS CONDICIONAIS ACIMA E PARTE PARA A QUERY
    //db, query, callback, args, error msg
    int ST = sqlite3_exec(dbConf.db, query, callback, 0, &zErrMsg);
    if (ST){
        string update    = "update";
        string to_search(query);

        if (to_search.find(update) != std::string::npos){
            ostringstream ss;
            ss << ST;

            debug("\n");
            debug("\033[1;240m (db_query) Erro ao executar a query: ");
            debug(query);
            debug("\n");
            debug("Codigo de erro: ");
            debug(ss.str());
            debug("\033[0m\n");

            log(" (db_query) Erro ao executar a query:");
            log(query);
            log("Base de dados:");
            log(dbConf.hs_database);
            string db_e = "Codigo de erro: ";

            db_e = db_e + ss.str();
            log(db_e);
            //MANTER A BASE ABERTA ENQUANTO O PROGRAMA ESTIVER RODANDO
            /*Fechar  base a cada conexao gerou um numero excessivo de arquivos descritores e consumo de memoria.
             * A base e fechada ao usar Ctrl+C para sair do programa.*/
            //sqlite3_close(dbConf.db);
            dbConf.query_status = ST;
            return;
        }
        else{
            dbConf.query_status = -1;
            return;
        }
    }
    dbConf.query_status = 0;
}

static int callback(void *NotUsed, int argLen, char **query, char **azColName){
    cout << "####----####" << endl;
    for(int i=0; i<argLen; i++){
        printf("%s=%s\n", azColName[i], query[i] ? query[i] : "NULL");
        dbConf.result = query[i];
    }
    printf("\n");
    return 0;
}

void clearClients(){
    for (int i=0; i<256; i++){
        for (int j=0; j<5; j++){
            clients[i][j] = 0;
        }
    }
}

//!Manipulador de tempo para se assemelhar ao millis do Arduino. Utilizado em conjunto com o millis declarado seguidamente a essa funcao.
int getMillis()
{
    timeb tb;
	ftime(&tb);
	int nCount = tb.millitm + (tb.time & 0xfffff) * 1000;
	return nCount;
}
//!Manipulador de tempo para se assemelhar ao millis do Arduino
int millis(int val)
{
    int nSpan = getMillis() - val;
	if(nSpan < 0)
		nSpan += 0x100000 * 1000;
	return nSpan;
}

void debug(string msg){
    if (DEBUG_CONSOLE == 1){
    cout << msg;
    }
}

//!Manipulador da interrupcao com Ctrl+C
void sig_handler(int sig)
{
  printf("\n%s Break received, exiting!\n", __BASEFILE__);
  log("Programa encerrado");
  //FECHA A BASE DE DADOS SE FOI SOLICITADO O ENCERRAMENTO DO PROGRAMA
  sqlite3_close(dbConf.db);

  force_exit=true;
}

/*! Funcao principal. Todo o programa e executado dentro dessa funcao. A primeira tarefa e validar o usuário; se
nao for root, sai imediatamente. Executar como root e mandatorio devido a acessos privilegiados do BCM.*/
int main (int argc, char *argv[] ){
    isAlreadyRunning();

    //Informacoes sobre a tabela da base de exemplo ficticia
    //////////////////////////////////////////////////////////////
    //sqlite> .header on                                        //
    //sqlite> .mode column                                      //
    //sqlite> pragma table_info('clients');                     //
    //////////////////////////////////////////////////////////////

    cout << string(100,'\n');

    /*O BCM faz acesso exclusivos que nao podem ser manipulados pelo usuário, portanto e necessário verificar quem o está executando para evitar
     * mensagens de erros que parecam-se com bug. O programa nao executa como usuário mesmo sem essa validacao.*/
    if (getuid()){
        cout << "O programa deve ser rodado como root. Saindo..." << endl;
        exit(0);
    }

    //!Utilizando boost para a leitura de arquivo ini com as predefinicoes de inicializacao.
    if (FILE *f = fopen(INIFILE,"r")){
        fclose(f);
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(INIFILE, pt);
        
        /*Se nao houver definicao do arquivo ini, utiliza os proprios valores definidos no array, no início das declaracoes do codigo.*/
        frequencies[0]       = pt.get<float>("frequency.radio0", frequencies[0]); //!Frequencia do rádio 0 configurado no arquivo /home/pi/AFMultiRadio.ini
        frequencies[1]       = pt.get<float>("frequency.radio1", frequencies[1]); //!Frequencia do rádio 1 configurado no arquivo /home/pi/AFMultiRadio.ini
        frequencies[2]       = pt.get<float>("frequency.radio2", frequencies[2]); //!Frequencia do rádio 2 configurado no arquivo /home/pi/AFMultiRadio.ini
        
        logdir_name          = pt.get<string>("debug.logsdir", "/dev/shm/AFMultiRadio.log"); //!Seleciona diretorio de logs, configurado no arquivo /home/pi/AFMultiRadio.ini
        logger               = pt.get<uint8_t>("debug.logger", 1); //!Ativa (ou nao) o log, configurado no arquivo /home/pi/AFMultiRadio.ini
        DEBUG_CONSOLE        = pt.get<uint8_t>("debug.verbose", 0); //!Exibe mensagem de debug em modo verboso, configurado no arquivo /home/pi/AFMultiRadio.ini
        
        services.listener[0] = pt.get<bool>("listener.radio0", true); //!Escuta por clients no rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.listener[1] = pt.get<bool>("listener.radio1", true); //!Escuta por clients no rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.listener[2] = pt.get<bool>("listener.radio2", true); //!Escuta por clients no rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        
        services.sender[0]   = pt.get<bool>("sender.radio0",true); //!Envio de firmware pelo rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.sender[1]   = pt.get<bool>("sender.radio1",false); //!Envio de firmware pelo rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.sender[2]   = pt.get<bool>("sender.radio2",false); //!Envio de firmware pelo rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        
        services.bridge[0]   = pt.get<bool>("bridge.radio0", false); //!Transferencia entre concentradores, pelo rádio 0, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.bridge[1]   = pt.get<bool>("bridge.radio1", false); //!Transferencia entre concentradores, pelo rádio 1, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)
        services.bridge[2]   = pt.get<bool>("bridge.radio2", true); //!Transferencia entre concentradores, pelo rádio 2, configurado no arquivo /home/pi/AFMultiRadio.ini (boolean)

        number_of_radios_to_initialize = pt.get<uint8_t>("start.initialize", 3); //Define o número de rádios que tem a placa. Os rádios serão inicializados na ordem u2, u3 e u7. Na ausência do parâmetro, os 3.

        string dname    = pt.get<string>("database.dbname");
        strncpy(dbConf.hs_database,dname.c_str(),dname.length());
    }
    else{
        memset(dbConf.hs_database,0,40);
        strcpy(dbConf.hs_database,(char *)"/dev/shm/AFMultiRadio.sqlite3");
        /*! Por padrao, o log e desabilitado, mas se for modificada a variável logger,
        já está garantido um nome de diretorio para evitar bugs. */
        logdir_name = "/home/pi/AFMultiRadio/AFMultiRadio.log";
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
    opterr  = 0;

    float t;
    int pos = 0;

    char *fq = NULL;
    while ((c = getopt(argc,argv,"vdfh")) != -1)

        switch(c){
        case 'h':
            hflag = 1;
            cout << endl;
            cout << "Usage: ";
            cout << argv[0] << endl;
            cout << "Params:" << endl;
            cout << "-v    Verbose execution" << endl;
            cout << "-d    Select database name (default:/dev/shm/AFMultiRadio.sqlite3)" << endl;
            cout << "-f    Frequencies. Ex: 914.75,914.125,914.0" << endl;
            exit(0);

            case 'v':
                vflag         = 1;
                DEBUG_CONSOLE = 1;
                break;
            case 'd':
                //dbConf.hs_database = (char*)argv[optind];
                memset(dbConf.hs_database,0,40);
                char *dbn;
                dbn = argv[optind];
                strcpy(dbConf.hs_database,dbn);
                cout << "Nova base de dados escolhida: ";
                cout << dbConf.hs_database << endl;
                log("Base de dados alternada com -d");
                break;
            case 'f':
                fvalue = (char*)argv[optind];
                fq     = strtok(fvalue,",");

                while (fq != NULL){
                    //printf("::: %s :::\n",fq);

                    t                = atof(fq);
                    frequencies[pos] = t;
                    fq               = strtok(NULL,",");
                    pos++;
                }

            case '?':
                if (optopt == 'r' || optopt == 'd' || optopt == 'f' || optopt == 'u'){
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                }
                else if (isprint(optopt)){
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                }
                else if (optopt == '\x0'){
                    uint8_t x = 0; //so pra evitar esse bug ate descobrir a razao
                }
                else{
                    fprintf (stderr,"Unknown option character `\\x%x'.\n",optopt);
                }
                //return 1;
            default:
                //abort();
                int x = 1;
    }

    //######################### DATABASE ################################//
    //O exemplo da criacao da tabela presume um campo endereco, consumo, nivel de bateria, violacao de caixa e datetime vindo de um dispositivo remoto
    string create_table_clients = "create table if not exists clients(address integer primary key unique not null, consumption integer,battery integer,violation integer,dt integer);";
    db_query(create_table_clients.c_str(),dbConf.using_db);
    sleep(1);

    cout << "\033[1;33mUltima atualizacao: " << LAST_UPDATE << "\033[0m" << endl << endl <<endl;

    clearClients();

	_state = LoraServerState::INIT_CONSOLE;


    
    struct stat st;
    
	unsigned long _lastMessageMillis = 0;

    unsigned long led_blink = 0;

    signal(SIGINT, sig_handler);
    //printf( "%s\n", __BASEFILE__);

    if (!bcm2835_init()) {
        fprintf( stderr, "%s bcm2835_init() Failed\n\n", __BASEFILE__ );
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

  for (int i=0;i<number_of_radios_to_initialize;i++){
      if (!rf95[i].init()) {
            string m = "(radio " + to_string(i) + " ERROR!!!)";
          debug(m + "\n");
          log(m);
          fprintf( stderr, "\033[1;31m::: RF95 module init failed. Please, check the board :::\033[0m\n\n" );
      }
      else{
          string msg =  "\033[32mRadio " +  to_string(i) + " started. Setting up...\033[0m\n" ;
          debug(msg);
      }
  }


    // Adjust Frequency
    for (int i=0;i<number_of_radios_to_initialize;i++){
        debug("Settings of radio " + to_string(i) + "\n");

        rf95[i].setTxPower(13, false);

        debug("Setting up band (" + to_string(frequencies[i]) + "MHz)\n" );
        rf95[i].setFrequency(frequencies[i]);
        rf95[i].setPromiscuous(true);
        rf95[i].setModeRx();
    }

    debug("\n\nWaiting message!\n\n");

    h.connect("http://192.168.0.6:8001");

    //SE PRECISAR FAZER DEBUG APENAS NO PRIMEIRO RADIO,
    //BASTA COMENTAR O OPERADOR TERNARIO NO FINAL DO LOOP
    while (!force_exit) {

        if (rf95[radioNumber].available()) {
          // Should be a message for us now
          uint8_t buf[MESSAGE_LENGTH];
          uint8_t len   = sizeof(buf);

          _lastMessageMillis = getMillis();

          uint8_t from  = rf95[radioNumber].headerFrom();
          //uint8_t to    = rf95[radioNumber].headerTo();
          //uint8_t id    = rf95[radioNumber].headerId();
          //uint8_t flags = rf95[radioNumber].headerFlags();
          //int8_t rssi   = rf95[radioNumber].lastRssi();

          string head = "\033[1;7;95m";
          string foot = "\033[0m";
          
          if (rf95[radioNumber].recv(buf, &len)) {
              if (buf[BUF_START] == '$' && buf[BUF_END] == '\n'){
                  head = " ";
                  foot = " ";

                  debug(head);
                  debug("Chegou mensagem (radio ");
                  debug(to_string(radioNumber));
                  debug("): ");

                  debug("addr: ");
                  debug(to_string(from) + " ");
                  debug("buffer: ");
                  //comeca no 0 e termina no <6 (BUF_END = 5): (0) $ (1) counter (2) counter (3) bat (4) violation (5) \n)
                  for (int i=BUF_START;i<MESSAGE_LENGTH;i++){
                      debug( to_string(buf[i]) + " ");
                  }

                  int addr            = from;
                  int consumption     = buf[1] << 8|(buf[2]);
                  int bat             = buf[3];
                  int violation       = buf[4];
                  string dt           = dateTimeToStr(UNIX_EPOCH);
                  
                  //Para converter para datetime: select datetime(dt,'unixepoch','localtime') from clients where address=>endereco>;

                  string query_insert = "insert into clients(address,consumption,battery,violation,dt) values (" + to_string(addr);
                  query_insert        = query_insert + "," + to_string(consumption) + "," + to_string(bat) + ",";
                  query_insert        = query_insert + to_string(violation) + "," + dt + ")";

                  db_query(query_insert.c_str(),dbConf.db); //O retorno esta na estrutura dbConf.results e dbConf.query_status


                  if (dbConf.query_status != 0){
                      string query_update = "update clients set consumption=" + to_string(consumption);
                      query_update        = query_update + ",battery=" + to_string(bat);
                      query_update        = query_update + ",violation=" + to_string(violation) + ",dt=" + dt + " where address=" + to_string(addr);
                      dbConf.query_status = 0;

                      db_query(query_update.c_str(),dbConf.db);
                  }
                  debug("\n");
                  debug(foot);

              }
              else{
                debug(head);
                debug(" ::: IGNORANDO MENSAGEM A SEGUIR ::: \n");
                debug("Mensagem invalida - rádio ");
                debug(to_string(radioNumber));
                debug(" Addr: ");
                debug(to_string(from));
                debug(" Buffer: ");
                for (int i=BUF_START;i<MESSAGE_LENGTH;i++){
                      debug( to_string(buf[i]) + " ");
                  }
                  debug("\n");
                  debug(foot);
              }

          }
          else {
              debug("Erro ao receber mensagem :(\n");
              log("Erro na recepcao de mensagem");
          }

        }


        /*
         * Se nao for definido no arquivo ini um ou mais rádios para envio de firmware, essa condicional nao e executada nunca. Definicao:
         * [sender]
         * radio0=true
         * radio1=false
         * radio2=false
         * */
        else if ((millis(_lastMessageMillis)) > TIME_TO_UPDATE && services.sender[radioNumber]){
                _lastMessageMillis = getMillis();
        }

        // Tempo para execucao de outras tarefas
        // Pode ser aumentado ou reduzido, mas reduzir esse delay pode sobrecarregar a CPU
        // Recomendado nao mexer
        bcm2835_delay(5);

        //COMENTE ESSA LINHA PARA USAR APENAS O RADIO 0
        radioNumber = radioNumber > number_of_radios_to_initialize-2 ? 0 : radioNumber+1;

    }//while (!force_exit)

  printf( "\n%s Ending\n", __BASEFILE__ );
  bcm2835_close();
  return 0;
}
