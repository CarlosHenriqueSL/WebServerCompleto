#include <stdio.h>  // Biblioteca padrão para entrada e saída
#include <string.h> // Biblioteca manipular strings
#include <stdlib.h> // funções para realizar várias operações, incluindo alocação de memória dinâmica (malloc)

#include "pico/stdlib.h"  // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
#include "hardware/adc.h" // Biblioteca da Raspberry Pi Pico para manipulação do conversor ADC
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h" // Biblioteca para arquitetura Wi-Fi da Pico com CYW43

#include "lib/ssd1306.h"
#include "lib/numeros.h"
#include "blink.pio.h"

#include "lwip/pbuf.h"  // Lightweight IP stack - manipulação de buffers de pacotes de rede
#include "lwip/tcp.h"   // Lightweight IP stack - fornece funções e estruturas para trabalhar com o protocolo TCP
#include "lwip/netif.h" // Lightweight IP stack - fornece funções e estruturas para trabalhar com interfaces de rede (netif)

// Credenciais WIFI - Tome cuidado se publicar no github!
#define WIFI_SSID "SEU_SSID"
#define WIFI_PASSWORD "SUA_SENHA"

#define WS2812_PIN 7

#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define ENDERECO 0x3C

volatile bool system_on = true; // true = ligado, false = desligado
volatile int irrigation_value = 0;

uint32_t matrix_rgb(double r, double g, double b);

void desenho_pio(double *desenho, PIO pio, uint sm, double r, double g, double b);

double *definir_desenho(double *desenho);

void desligar(ssd1306_t *ssd, PIO pio, uint sm, bool *previous_system_on);

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);

// Leitura da temperatura interna
float temp_read(void);

// Tratamento do request do usuário
void user_request(char **request);

// Função principal
int main()
{
    // Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
    stdio_init_all();

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_t ssd;
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, ENDERECO, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &blink_program);
    uint sm = pio_claim_unused_sm(pio, true);
    blink_program_init(pio, sm, offset, WS2812_PIN);

    // Inicializa a arquitetura do cyw43
    while (cyw43_arch_init())
    {
        printf("Falha ao inicializar Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }

    // Ativa o Wi-Fi no modo Station, de modo a que possam ser feitas ligações a outros pontos de acesso Wi-Fi.
    cyw43_arch_enable_sta_mode();

    // Conectar à rede WiFI - fazer um loop até que esteja conectado
    printf("Conectando ao Wi-Fi...\n");
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000))
    {
        printf("Falha ao conectar ao Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }
    printf("Conectado ao Wi-Fi\n");

    // Caso seja a interface de rede padrão - imprimir o IP do dispositivo.
    if (netif_default)
    {
        printf("IP do dispositivo: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    }

    // Configura o servidor TCP - cria novos PCBs TCP. É o primeiro passo para estabelecer uma conexão TCP.
    struct tcp_pcb *server = tcp_new();
    if (!server)
    {
        printf("Falha ao criar servidor TCP\n");
        return -1;
    }

    // vincula um PCB (Protocol Control Block) TCP a um endereço IP e porta específicos.
    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("Falha ao associar servidor TCP à porta 80\n");
        return -1;
    }

    // Coloca um PCB (Protocol Control Block) TCP em modo de escuta, permitindo que ele aceite conexões de entrada.
    server = tcp_listen(server);

    // Define uma função de callback para aceitar conexões TCP de entrada. É um passo importante na configuração de servidores TCP.
    tcp_accept(server, tcp_server_accept);
    printf("Servidor ouvindo na porta 80\n");

    // Inicializa o conversor ADC
    adc_init();
    adc_set_temp_sensor_enabled(true);

    bool previous_system_on = false; // Variável para detectar a transição de desligado para ligado

    while (true)
    {
        // Verifica se o sistema foi desligado
        if (!system_on)
        {
            desligar(&ssd, pio, sm, &previous_system_on);
            sleep_ms(100); 
            continue;      // Pula o restante do loop
        }

        // Verifica se o sistema foi reiniciado
        if (!previous_system_on)
        {
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Sistema iniciado", 4, 25);
            ssd1306_send_data(&ssd);
            sleep_ms(2000); // Espera 2 segundos
            previous_system_on = true;
            irrigation_value = 0;
        }

        char irrigation_str[40];
        snprintf(irrigation_str, sizeof(irrigation_str), "Intensidade da \n\n\n irrigacao: %d%%", irrigation_value);

        ssd1306_fill(&ssd, false);
        ssd1306_draw_string(&ssd, irrigation_str, 4, 25);
        ssd1306_send_data(&ssd);

        double *desenho = definir_desenho(NULL);            // Corrige ponteiro não inicializado
        double intensity = irrigation_value / 100.0;        // Converte para valor entre 0.0 e 1.0
        desenho_pio(desenho, pio, sm, intensity, 0.0, 0.0); // Mantém apenas o componente vermelho, mas ajusta a intensidade

        /*
         * Efetuar o processamento exigido pelo cyw43_driver ou pela stack TCP/IP.
         * Este método deve ser chamado periodicamente a partir do ciclo principal
         * quando se utiliza um estilo de sondagem pico_cyw43_arch
         */
        cyw43_arch_poll(); // Necessário para manter o Wi-Fi ativo
        sleep_ms(100);     // Reduz o uso da CPU
    }

    // Desligar a arquitetura CYW43.
    cyw43_arch_deinit();
    return 0;
}

// -------------------------------------- Funções ---------------------------------

// Envia padrão para a matriz de LEDs
uint32_t matrix_rgb(double r, double g, double b)
{
    unsigned char R = r * 255;
    unsigned char G = g * 255;
    unsigned char B = b * 255;
    // WS2812 espera os dados no formato GRB nos 24 bits mais significativos
    return (G << 24) | (R << 16) | (B << 8);
}

// Envia padrão para a matriz de LEDs
void desenho_pio(double *desenho, PIO pio, uint sm, double r, double g, double b)
{
    for (int i = 0; i < NUM_PIXELS; i++)
    {
        // Usa o valor do desenho para controlar a intensidade de todas as cores
        double intensity = desenho[24 - i];
        uint32_t led_val = matrix_rgb(intensity * r, intensity * g, intensity * b);
        pio_sm_put_blocking(pio, sm, led_val);
    }
}

// Seleciona o desenho que sera exibido na matriz
double *definir_desenho(double *desenho)
{
    // Encontra o desenho mais próximo baseado no valor de irrigação
    if (irrigation_value < 10)
        desenho = zeroPorcento;
    else if (irrigation_value < 30)
        desenho = vintePorcento;
    else if (irrigation_value < 50)
        desenho = quarentaPorcento;
    else if (irrigation_value < 70)
        desenho = sessentaPorcento;
    else if (irrigation_value < 90)
        desenho = oitentaPorcento;
    else
        desenho = cemPorcento;
    
    return desenho;
}

// Desliga o sistema
void desligar(ssd1306_t *ssd, PIO pio, uint sm, bool *previous_system_on)
{
    // Limpa o display
    ssd1306_fill(ssd, false);
    ssd1306_send_data(ssd);
    // Apaga a matriz de LEDs (envia 0 para todos os pixels)
    for (int i = 0; i < NUM_PIXELS; i++)
    {
        pio_sm_put_blocking(pio, sm, 0);
    }
    *previous_system_on = false; // Atualiza a flag no escopo principal
    sleep_ms(100);
}

// Função de callback ao aceitar conexões TCP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

// Tratamento do request do usuário - digite aqui
void user_request(char **request)
{
    if (strstr(*request, "GET /system_on") != NULL)
    {
        system_on = true;
    }
    else if (strstr(*request, "GET /system_off") != NULL)
    {
        system_on = false;
    }
    else if (strstr(*request, "GET /set_irrigation?irrigation=") != NULL)
    {
        // Extrai o valor do parâmetro irrigation
        char *start = strstr(*request, "irrigation=") + strlen("irrigation=");
        char *end = strchr(start, ' ');
        if (end == NULL)
            end = strchr(start, '\0');

        char value_str[4] = {0}; // Máximo 3 dígitos + terminador nulo
        strncpy(value_str, start, end - start);

        // Converte para inteiro e atualiza a variável global
        irrigation_value = atoi(value_str);

        // Limita o valor entre 0 e 100
        if (irrigation_value < 0)
            irrigation_value = 0;
        if (irrigation_value > 100)
            irrigation_value = 100;

        printf("Valor de irrigação definido para: %d\n", irrigation_value);
    }
};

// Função de callback para processar requisições HTTP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (!p)
    {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        return ERR_OK;
    }

    // Alocação do request na memória dinámica
    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';

    printf("Request: %s\n", request);

    // Tratamento de request - Controle dos LEDs
    user_request(&request);

    // Cria a resposta HTML
    char html[1024];

    // Instruções html do webserver
    snprintf(html, sizeof(html),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n"
             "\r\n"
             "<!DOCTYPE html>\n"
             "<html>\n"

             "<head>\n"
             "<meta charset=\"UTF-8\">\n"
             "<title>Irrigation Lab</title>\n"

             "<style>\n"
             "body { background-color: rgba(247, 86, 86, 0.93); font-family: Arial, sans-serif; text-align: center; margin-top: 50px; }\n"
             "h1 { font-size: 64px; margin-bottom: 30px; }\n"
             "button { background-color: rgba(152, 194, 192, 0.93); font-size: 36px; margin: 20px; padding: 20px 40px; border-radius: 20px; }\n"
             "label { font-size: 24px; display: block; margin-bottom: 1px; margin-top: 10px; }\n"
             "</style>\n"

             "</head>\n"

             "<body>\n"
             "<h1>Irrigation Lab</h1>\n"

             "<form action=\"./system_off\"><button>Desligar sistema</button></form>\n"
             "<form action=\"./system_on\"><button>Ligar sistema</button></form>\n"

             "<form action=\"./set_irrigation\" method=\"GET\">\n"
             "<label for=\"irrigation\">Intensidade da irrigação</label>\n"
             "<input type=\"range\" id=\"irrigation\" name=\"irrigation\" min=\"0\" max=\"100\" value=\"50\">\n"
             "<button type=\"submit\">Enviar Irrigação</button>\n"
             "</form>\n"

             "</body>\n"
             "</html>\n");

    // Escreve dados para envio (mas não os envia imediatamente).
    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);

    // Envia a mensagem
    tcp_output(tpcb);

    // libera memória alocada dinamicamente
    free(request);

    // libera um buffer de pacote (pbuf) que foi alocado anteriormente
    pbuf_free(p);

    return ERR_OK;
}
