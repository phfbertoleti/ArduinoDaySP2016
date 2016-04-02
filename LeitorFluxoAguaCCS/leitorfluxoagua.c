//Firmware leitor de fluxo de �gua (demonstra��o da palestra "Embedded Systems e IoT: do bare-metal � comunica��o wireless segura", realizado no QCON 2016)
//Autor: Pedro Bertoleti
//Data: 02/2016
//IMPORTANTE: deve ser compilado com o compilador CCS v4.093


#include <18F4520.h>
#include "HeaderLeitor.h"
#use delay(clock=4000000) 	//Clock de 4MHz (clock �til de 1MHz)
#fuses WDT128,HS,PUT,NOPROTECT
#use rs232(baud=9600,parity=N,xmit=PIN_C6,rcv=PIN_C7,bits=8,ERRORS)
#priority INT_TIMER1, INT_EXT, INT_RDA  //ordem de prioridad das interrup��es (ordem decrescente de prioridade)

//defines gerais: 

//sa�da (breathing light)
#define BREATHING_LIGHT             PIN_A0
#define TAMANHO_MSG_RESPOSTA        sizeof(TDadosProtocoloLeitorAgua) + 2
#define TAMANHO_CHAVE_EEPROM        sizeof(ChaveEEPROM)
#define TAMANHO_VERSAO              sizeof(VersaoEquipamento)-1
#define TAMANHO_VAZAO_ASCII         6
#define TAMANHO_CONSUMO_ASCII       6

//prototypes
void ConfigInterrupcaoEXT(void);
void ConfigInterrupcaoUART(void);
void ConfigTimer1(void);
void TrataMensagem(void);
void LeConsumoAcumulado(void);
void LeCalibracaoSensor(void);
void SalvaCalibracaoSensor(void);
void ResetConsumoAcumulado(void);
void ResetCalibracaoSensor(void);
char VerificaChaveEEPROM(char EnderecoInicialChave);
void MontaEEnviaMensagem(char Opcode, char Tamanho, char * Dado);
void MontaEEnviaMensagemNula(char Opcode);
void AguardaSTX();
void AguardaEndereco(char ByteRecebido);
void AguardaOpcode(char ByteRecebido);
void AguardaTamanho(char ByteRecebido);
void AguardaCheckSum(char ByteRecebido);
void AguardaBuffer(char ByteRecebido);
char CalculaCheckSum(void);
void MaquinaEstadoSerial(char ByteRecebido);


//tratamento da interrup��o externa
#int_EXT 
void  EXT_isr(void) 
{ 
	ContadorPulsos++;    
} 

//tratamento da interrup��o serial
#INT_RDA
void serial_isr()
{ 
    char ByteLido;
    ByteLido = getc();
    MaquinaEstadoSerial(ByteLido);
} 

//tratamento da interrup��o de timer
#INT_TIMER1
void TrataTimer1()
{
	ContadorIntTimer++;

    if (ContadorIntTimer < 5)   //cada "tick" do timer1 tem 0,2s. Logo, 5 "tiks" equivalem a 1 segundo
    {
        set_timer1(15536);
		return;
	}

	if (EstaEmModoCalibracao == SIM)  //se o equipamento est� em modo calibra��o, nada deve ser feito aqui
	{
		set_timer1(15536);
		return;
	}
	
	if (PulsosPorLitro == 0) //n�o h� calibra��o do sensor realizada. Nenhum calculo � feito
	{
		VazaoCalculada = 0;	
		memset(VazaoAscii,0,sizeof(VazaoAscii));
    	sprintf(VazaoAscii,"%6.2f",VazaoCalculada);

		ConsumoCalculado = 0;
		memset(ConsumoAscii,0,sizeof(ConsumoAscii));
    	sprintf(ConsumoAscii,"%6.2f",ConsumoCalculado);

		set_timer1(15536);
		return;
	}
	
    //1 segundo se passou
    ContadorIntTimer=0;
    TempoSalvamentoConsumo++;

   	//deliga todas interrup��es
    disable_interrupts(INT_RDA);
    disable_interrupts(INT_EXT);

    //calcula vaz�o em l/h
    FrequenciaCalculada = ContadorPulsos;
	VazaoCalculada = 0;

    /*
                Pulsos                    Volume     Tempo
               	PulsosPorLitro      ---     1l   --- 1s
				FrequenciaCalculada ---      x   --- 1s 
                
				x = FrequenciaCalculada / PulsosPorLitro
				
    */
    VazaoCalculada = (float)((float)FrequenciaCalculada / (float)PulsosPorLitro);  //l/s
	
    ConsumoCalculado = ConsumoCalculado + VazaoCalculada;    //contabiliza consumo deste periodo
	VazaoCalculada = VazaoCalculada*3600.0;   //l/h

	memset(VazaoAscii,0,sizeof(VazaoAscii));
    sprintf(VazaoAscii,"%6.2f",VazaoCalculada);

    //calcula consumo e o formata para ascii
    memset(ConsumoAscii,0,sizeof(ConsumoAscii));
    sprintf(ConsumoAscii,"%6.2f",ConsumoCalculado);
	
    //verifica se deve salvar na eeprom o consumo total
    if (TempoSalvamentoConsumo >= TEMPO_SALVAMENTO_CONSUMO_SEGUNDOS)  
		DeveGravarConsumo = SIM;
	
    //troca o estado da sa�da do breathing light
	output_toggle(BREATHING_LIGHT);
     
    //configura timer e religa interrup��es	
    set_timer1(15536);
    ConfigInterrupcaoUART();
    ConfigInterrupcaoEXT();	
}

//Aguarda STX - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaSTX(char ByteRecebido)
{
    if (ByteRecebido == STX)
	{
		memset(&DadosProtocoloLeitorAgua, 0, sizeof(TDadosProtocoloLeitorAgua));   //limpa dados do protocolo
		EstadoSerial = ESTADO_ENDERECO;
	}
	else
		EstadoSerial = ESTADO_STX;
}

//Aguarda endere�o do destinat�rio da mensagem - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaEndereco(char ByteRecebido)
{
    DadosProtocoloLeitorAgua.Endereco = ByteRecebido;
	
	if (DadosProtocoloLeitorAgua.Endereco == ENDERECO_BAREMETAL)
		EstadoSerial = ESTADO_OPCODE;	
	else
		EstadoSerial = ESTADO_STX;	
}

//Aguarda Opcode da mensagem - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaOpcode(char ByteRecebido)
{
    DadosProtocoloLeitorAgua.Opcode = ByteRecebido;
	EstadoSerial = ESTADO_TAMANHO;	
}

//Aguarda tamanho da mensagem - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaTamanho(char ByteRecebido)
{
	if (ByteRecebido > MAX_TAM_BUFFER)
		EstadoSerial = ESTADO_STX;   //tamanho recebido � inv�lido (maior que o m�ximo permitido). A m�quina de estados � resetada.
	else
	{	
		DadosProtocoloLeitorAgua.TamanhoMensagem = ByteRecebido;
		EstadoSerial = ESTADO_CHECKSUM;	
	}
}

//Aguarda checksum da mensagem - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaCheckSum(char ByteRecebido)
{	
    DadosProtocoloLeitorAgua.CheckSum = ByteRecebido;
	if(DadosProtocoloLeitorAgua.TamanhoMensagem > 0)
	{
		IndiceBuffer = 0;
		EstadoSerial = ESTADO_BUFFER;
	}
	else
	{
		RecebeuBufferCompleto = 1;
		EstadoSerial = ESTADO_STX;
	}
}

//Aguarda buffer da mensagem - fun��o da m�quina de estados da comunica��o serial
//parametros: byte recebido
//saida: nenhum
void AguardaBuffer(char ByteRecebido)
{
    if(IndiceBuffer < DadosProtocoloLeitorAgua.TamanhoMensagem)
	{
		DadosProtocoloLeitorAgua.Buffer[IndiceBuffer] = ByteRecebido;
		IndiceBuffer++;
	}
	else
	{
		//buffer completo. Faz o tratamento da mensagem e reinicia m�quina de estados
		if (CalculaCheckSum() == CHECKSUM_OK)
			RecebeuBufferCompleto = RECEPCAO_OK;
		else
            RecebeuBufferCompleto = SEM_RECEPCAO;
	
		EstadoSerial = ESTADO_STX;
	}
}

//Checa o checksum da mensagem recebida pela UART
//parametros: nenhum
//saida: nenhum
char CalculaCheckSum(void)
{
	char CheckSumCalculado;
	char i;
	
	CheckSumCalculado = 0;
	
	for(i=0; i<DadosProtocoloLeitorAgua.TamanhoMensagem; i++)
		CheckSumCalculado = CheckSumCalculado + DadosProtocoloLeitorAgua.Buffer[i];
		
	CheckSumCalculado = (~CheckSumCalculado) +1;
	
	if (CheckSumCalculado == DadosProtocoloLeitorAgua.CheckSum)
		return CHECKSUM_OK;
	else
		return FALHA_CHECKSUM;
}


//Fun��o que faz o gerenciamento dos estados da m�quina de estado. � chamada sempre que um byte chega da serial
//parametros: byte recebido pela serial
//saida: nenhum
void MaquinaEstadoSerial(char ByteRecebido)
{
	switch(EstadoSerial)
    {
        case ESTADO_STX:
        {
            AguardaSTX(ByteRecebido);
            break; 
        }

		case ESTADO_ENDERECO:
        {
            AguardaEndereco(ByteRecebido);
            break; 
        }
		
        case ESTADO_OPCODE:
        {
            AguardaOpcode(ByteRecebido);
            break; 
        }

        case ESTADO_TAMANHO:
        {
            AguardaTamanho(ByteRecebido);
            break; 
        }

        case ESTADO_CHECKSUM:
        {
            AguardaCheckSum(ByteRecebido);
            break; 
        }

        case ESTADO_BUFFER:
        {
            AguardaBuffer(ByteRecebido);
            break; 
        }

        
        default:   //se o estado tiver qualquer valro diferente dos esperados, significa que algo corrompeu seu valor (invas�o de mem�ria RAM). Logo a m�quina de estados � reuniciada.
        {
            EstadoSerial=ESTADO_STX;
            RecebeuBufferCompleto = SEM_RECEPCAO;
            memset(&DadosProtocoloLeitorAgua, 0, sizeof(TDadosProtocoloLeitorAgua));   //limpa dados do protocolo
            break;
        }
    }
}


//fun��o de configura��o da interrup��o UART
//parametros: nenhum
//saida: nenhum
void ConfigInterrupcaoUART(void)
{    
    enable_interrupts(INT_RDA);
    enable_interrupts(GLOBAL);
}

//fun��o de configura��o da interrup��o externa
//parametros: nenhum
//saida: nenhum
void ConfigInterrupcaoEXT(void)
{    
    ContadorPulsos = 0;
    enable_interrupts(INT_EXT);
    enable_interrupts(GLOBAL);
    ext_int_edge(L_TO_H);
}


//fun��o de configura��o do Timer1
//parametros: nenhum
//saida: nenhum
void ConfigTimer1(void)
{
    // - Frequencia do oscilador interno (4000000/4)=1Mhz (por default, o PIC funciona a 1/4 da frequencia de clock estabelecida)
	// - Se o Timer1 tem 16 bits, seu valor m�ximo de contagem � 0xFFFF (65535)	
	// - Com 1MHz de frequencia util, temos que cada ciclo de m�quina ter�, em segundos: 1 / 1MHz = 0,000001 (1us)
    // - Utilizando o prescaler do microcontrolador em 4 (ou seja, a frequencia util do timer1 � 1/4 da frequencia util do pic), temos:
    //   Periodo minimo "cont�vel" pelo Timer1 =  (1 / (1MHz/4))   = 0,000004 (4us)
    // - Logo, a cada 16 bits contados, teremos: 65536 * 4us =  0,262144s
    // - visando maior precis�o, sera feito um timer de 0,2s. Logo:   
    //              0,262144s   ---  65536
    //                 0,20s     ---     x        x = 50000
    // Logo, o valor a ser setao no timer1 �: 65536 - 50000 = 15536
 
    ContadorIntTimer=0;
    setup_timer_1(T1_INTERNAL| T1_DIV_BY_4);
    set_timer1(15536);
    enable_interrupts(INT_TIMER1);      
    enable_interrupts(GLOBAL);
 
}

//formata e envia a mensagem (em resposta � solicita��o)
//parametros: opcode, tamanho, ponteiro pro dado ascii
//saida: nenhum
void MontaEEnviaMensagem(char Opcode, char Tamanho, char * Dado)
{
    char BufferAscii[TAMANHO_MSG_RESPOSTA];
    char CheckSumMsg;
    char i;
    char * ptrDado;

    CheckSumMsg = 0;

    //monta mensagem de resposta � solicita��o de consumo
    memset(BufferAscii,0,TAMANHO_MSG_RESPOSTA);
    BufferAscii[0]=STX;
	BufferAscii[1]=ENDERECO_BAREMETAL;
    BufferAscii[2]=Opcode; 
    BufferAscii[3]=Tamanho; 
   
    ptrDado = Dado;

    for(i=0; i<Tamanho; i++)
    {
       	CheckSumMsg = CheckSumMsg + (*ptrDado);
		ptrDado++;
    }

    CheckSumMsg = (~CheckSumMsg) + 1;
    memcpy(BufferAscii+5,Dado,Tamanho);
   	BufferAscii[4]=CheckSumMsg;

    //envio da mensagem
    puts(BufferAscii); 

} 

//Fun��o:formata e envia a mensagem nula (em resposta � solicita��o de comandos que n�o precisam retornar dados). 
//       entende-se por mensagem nula o envio de um espa�o (0x20) na parte de dados do protocolo
//parametros: opcode
//saida: nenhum
void MontaEEnviaMensagemNula(char Opcode)
{
	char BufferAscii[TAMANHO_MSG_RESPOSTA];
    
    memset(BufferAscii,0,TAMANHO_MSG_RESPOSTA);
    BufferAscii[0]=STX;
    BufferAscii[1]=Opcode; 
    BufferAscii[2]=0x01;
    BufferAscii[4]=0x20;
	BufferAscii[3]=0xE0;  //((~0x20) + 1)
    
    //envio da mensagem 
    puts(BufferAscii); 
}

//Trata mensagem recebida pela UART
//parametros: nenhum
//saida: nenhum
void TrataMensagem(void)
{
    char DadoVazio;

    DadoVazio = 0x00;

    switch(DadosProtocoloLeitorAgua.Opcode)
	{
		case OPCODE_RESET_CONSUMO:
		{
		    ResetConsumoAcumulado();
			MontaEEnviaMensagemNula(OPCODE_RESET_CONSUMO);
			break;
		}
		
		case OPCODE_LEITURA_VAZAO:
		{
		    MontaEEnviaMensagem(OPCODE_LEITURA_VAZAO, TAMANHO_VAZAO_ASCII,(char *)VazaoAscii);
            break;
		}
		
		case OPCODE_LEITURA_VERSAO:
		{
		    MontaEEnviaMensagem(OPCODE_LEITURA_VERSAO, TAMANHO_VERSAO,(char *)VersaoEquipamento);
			break;
		}

		case OPCODE_LEITURA_CONSUMO:
		{
		    MontaEEnviaMensagem(OPCODE_LEITURA_CONSUMO, TAMANHO_CONSUMO_ASCII,(char *)ConsumoAscii);
			break;
		}

		case OPCODE_ENTRA_EM_CALIBRACAO:
		{
			output_bit(BREATHING_LIGHT,1);
         	EstaEmModoCalibracao = SIM;
            MontaEEnviaMensagemNula(OPCODE_ENTRA_EM_CALIBRACAO);
			break;
		}
		
		case OPCODE_SAI_DA_CALIBRACAO:
		{
			SalvaCalibracaoSensor();
			EstaEmModoCalibracao = NAO;
			MontaEEnviaMensagemNula(OPCODE_SAI_DA_CALIBRACAO);
			break;
		}
		
        default:
        {
            RecebeuBufferCompleto = SEM_RECEPCAO;
          	EstadoSerial = ESTADO_STX;
			
			//limpa dados do protocolo 
            memset(&DadosProtocoloLeitorAgua, 0, sizeof(TDadosProtocoloLeitorAgua));  
            break;
        }
	}
}

//Zera consumo acumulado
//parametros: nenhum
//saida: nenhum
void ResetConsumoAcumulado(void)
{	
	char i;
	char EnderecoEscritaEEPROM;
	
    //deliga todas interrup��es
    disable_interrupts(INT_RDA);
    disable_interrupts(INT_EXT);


	ConsumoCalculado=0;
	ConsumoAscii[0]=' ';
    ConsumoAscii[1]='0';
    ConsumoAscii[2]='.';
    ConsumoAscii[3]='0';
    ConsumoAscii[4]='0';
    ConsumoAscii[5]=0;
    ConsumoAscii[6]=0;
    
	EnderecoEscritaEEPROM = ENDERECOCHAVE;
	
	//grava a chave na EEPROM
	for(i=0;i<sizeof(ChaveEEPROM);i++)
	{
		write_eeprom(EnderecoEscritaEEPROM,ChaveEEPROM[i]);
		EnderecoEscritaEEPROM++;
	}	
	
	//grava consumo
	for (i = 0; i < 4; i++) 
	{
		write_eeprom(EnderecoEscritaEEPROM, *((int8*)&ConsumoCalculado + i) ) ; 
		EnderecoEscritaEEPROM++;
    }	

    //configura timer e religa interrup��es	
    set_timer1(15536);
    ConfigInterrupcaoUART();
    ConfigInterrupcaoEXT();
}

//Grava que equipamento est� sem calibra��o
//parametros: nenhum
//saida: nenhum
void ResetCalibracaoSensor(void)
{	
	char i;
	char EnderecoEscritaEEPROM;
	
    //deliga todas interrup��es
    disable_interrupts(INT_RDA);
    disable_interrupts(INT_EXT);

	PulsosPorLitro = 0;
	ConsumoAscii[0]=' ';
    ConsumoAscii[1]='0';
    ConsumoAscii[2]='.';
    ConsumoAscii[3]='0';
    ConsumoAscii[4]='0';
    ConsumoAscii[5]=0;
    ConsumoAscii[6]=0;
    
	EnderecoEscritaEEPROM = ENDERECOCALIBRACAO;
	
	//grava a chave na EEPROM
	for(i=0;i<sizeof(ChaveEEPROM);i++)
	{
		write_eeprom(EnderecoEscritaEEPROM,ChaveEEPROM[i]);
		EnderecoEscritaEEPROM++;
	}	
	
	//grava consumo
	for (i = 0; i < 4; i++) 
	{
		write_eeprom(EnderecoEscritaEEPROM, *((int8*)&PulsosPorLitro + i) ) ; 
		EnderecoEscritaEEPROM++;
    }

	//configura timer e religa interrup��es	
    set_timer1(15536);
    ConfigInterrupcaoUART();
    ConfigInterrupcaoEXT();
	
}



//Verifica se a chave da EEPROM est� gravada
//parametros: endere�o inicial da chave procurada
//saida: 0 - chave n�o est� gravada
//       1 - chave est� gravada 
char VerificaChaveEEPROM(char EnderecoInicialChave)
{
	char i;
    char Iguais;
	
    Iguais=0;
	//le da EERPOM a chave gravada
	for(i=0; i<TAMANHO_CHAVE_EEPROM; i++)
    {
		if (read_eeprom(EnderecoInicialChave+i) == ChaveEEPROM[i])
			Iguais++;
    }
		
	if (Iguais == TAMANHO_CHAVE_EEPROM)	
		return SIM;
	else
		return NAO;
}

//Le consumo de �gua acumulado
//parametros: nenhum
//saida: nenhum
void LeConsumoAcumulado(void)
{
	char EnderecoLeituraEEPROM;
    char i;

	if (VerificaChaveEEPROM(ENDERECOCHAVE) == NAO)   //verifica se a EEPROM contem a chave gravada
		ResetConsumoAcumulado();
	else
	{	
		//a chave gravada est� correta. O consumo de �gua � lido.
		EnderecoLeituraEEPROM = ENDERECOCHAVE + TAMANHO_CHAVE_EEPROM;
		
	    for (i = 0; i < 4; i++) 
	    {
			*((int8*)&ConsumoCalculado + i) = read_eeprom(EnderecoLeituraEEPROM); 
			EnderecoLeituraEEPROM++;
        }
   	}
}

//Le calibra��o do sensor
//parametros: nenhum
//saida: nenhum
void LeCalibracaoSensor(void)
{
	char EnderecoLeituraEEPROM;
    char i;

	if (VerificaChaveEEPROM(ENDERECOCALIBRACAO) == NAO)   //verifica se a EEPROM contem a chave gravada
	{
		ResetCalibracaoSensor();
	}
	else
	{
		//a chave gravada est� correta. A calibra��o do sensor � lida
		EnderecoLeituraEEPROM = ENDERECOCALIBRACAO + TAMANHO_CHAVE_EEPROM;
		
	    for (i = 0; i < 2; i++) 
	    {
			*((int8*)&PulsosPorLitro + i) = read_eeprom(EnderecoLeituraEEPROM); 
			EnderecoLeituraEEPROM++;
        }
	}
}

//Salva calibra��o do sensor
//parametros: nenhum
//saida: nenhum
void SalvaCalibracaoSensor(void)
{
	char EnderecoEscritaEEPROM;
    char i;

    //deliga todas interrup��es
    disable_interrupts(INT_RDA);
    disable_interrupts(INT_EXT);
	
	PulsosPorLitro = ContadorPulsos;
	EnderecoEscritaEEPROM = ENDERECOCALIBRACAO;
	
	//grava a chave na EEPROM
	for(i=0;i<sizeof(ChaveEEPROM);i++)
	{
		write_eeprom(EnderecoEscritaEEPROM,ChaveEEPROM[i]);
		EnderecoEscritaEEPROM++;
	}	
	
	//grava consumo
	for (i = 0; i < 2; i++) 
	{
		write_eeprom(EnderecoEscritaEEPROM, *((int8*)&PulsosPorLitro + i) ) ; 
		EnderecoEscritaEEPROM++;
    }
	
	//configura timer e religa interrup��es	
    set_timer1(15536);
    ConfigInterrupcaoUART();
    ConfigInterrupcaoEXT();
}

//programa principal
void main(void)
{
    char i;
    char EnderecoEscritaEEPROM;
	
	//inicializa��o de vari�veis globais
    FrequenciaCalculada = 0;
    ContadorPulsos = 0;
    TempoSalvamentoConsumo = 0;
    RecebeuBufferCompleto = SEM_RECEPCAO;
 	EstadoSerial = ESTADO_STX; 
    DeveGravarConsumo = NAO;
	EstaEmModoCalibracao = NAO;
    EnderecoEscritaEEPROM = ENDERECOCHAVE;
	
    //liga breathing light
    output_bit(BREATHING_LIGHT,1);

	//faz a leitura do consumo acumulado de �gua
	LeConsumoAcumulado();
	
	//Le a calibra��o de pulsos gravada na eeprom
	LeCalibracaoSensor();
	
    //configura��o das interrup��es (UART e interrup��o externa)
 	ConfigInterrupcaoUART();
	ConfigInterrupcaoEXT();

    //configura Timer1 
    ConfigTimer1();

    setup_wdt(WDT_ON);
   
    while(1)
	{		
		restart_wdt();

        if (RecebeuBufferCompleto == RECEPCAO_OK)   //trata buffer recebido (visando otimiza��o de desempenho de interrup��o serial,  este tratamento � feito aqui)
		{
			RecebeuBufferCompleto=SEM_RECEPCAO;
            TrataMensagem();
		}
		
		//verifica se deve gravar consumo na EEPROM
		if ((DeveGravarConsumo == SIM) && (EstaEmModoCalibracao == NAO))
		{
			//deliga todas interrup��es
            disable_interrupts(INT_RDA);
            disable_interrupts(INT_EXT);

            //consumo deve ser gravado
			for (i = 0; i < 4; i++) 
			{
				write_eeprom(EnderecoEscritaEEPROM, *((int8*)&ConsumoCalculado + i) ) ; 
				EnderecoEscritaEEPROM++;
			}
			DeveGravarConsumo = NAO;
			TempoSalvamentoConsumo = 0;

			//configura timer e religa interrup��es	
            set_timer1(15536);
            ConfigInterrupcaoUART();
            ConfigInterrupcaoEXT();
		}
	}
}