# Audio Monitor — maurojuniorr Rebuild

Fork experimental do plugin **Audio Monitor para OBS Studio**, mantido por
[@maurojuniorr](https://github.com/maurojuniorr), com foco em melhorar a
estabilidade do monitoramento de áudio no macOS.

> [!IMPORTANT]
> Esta é uma versão comunitária e não oficial. Para o plugin original, suporte
> oficial e versões publicadas pelo autor, consulte o projeto do
> [exeldro](https://github.com/exeldro/obs-audio-monitor).

O plugin adiciona ao OBS Studio um filtro e uma dock de monitoramento. Com ele,
é possível encaminhar o áudio de uma fonte do OBS para um ou mais dispositivos
de saída.

![Audio Monitor no OBS Studio](media/screenshot.png)

## O que esta versão busca melhorar

- maior estabilidade ao monitorar a mesma fonte em duas saídas físicas;
- prevenção de travamentos relacionados ao `AudioQueue` no macOS;
- tratamento mais seguro ao desativar e reativar fontes;
- buffer limitado e pré-carregamento para reduzir pequenas interrupções;
- retomada suave do áudio para evitar estalos;
- proteção contra falhas e corridas durante a criação e remoção de saídas;
- tradução pt-BR revisada e identificação visual da rebuild.

As mudanças foram testadas no macOS com duas saídas físicas e ciclos repetidos
de desativação e reativação de fontes. Ainda assim, esta rebuild deve ser
considerada experimental.

## Download

Baixe a compilação universal mais recente para macOS:

**[Download do Audio Monitor — maurojuniorr Rebuild v5](https://github.com/maurojuniorr/obs-audio-monitor/releases/latest/download/audio-monitor-maurojuniorr-v5-universal.zip)**

Página da versão e notas de lançamento:

[0.10.1-maurojuniorr.5](https://github.com/maurojuniorr/obs-audio-monitor/releases/tag/0.10.1-maurojuniorr.5)

## Instalação no macOS

1. Feche completamente o OBS Studio.
2. Baixe e extraia o arquivo ZIP acima.
3. Copie o conteúdo para a pasta de plugins indicada no pacote.
4. Substitua a instalação anterior do Audio Monitor, se solicitado.
5. Abra o OBS Studio novamente.
6. Em uma fonte de áudio, abra **Filtros** e adicione **Audio Monitor**.
7. Selecione o dispositivo de saída desejado. Para uma segunda saída, adicione
   outro filtro Audio Monitor à mesma fonte.

Antes de substituir uma versão que já funciona, mantenha uma cópia do plugin
anterior para poder restaurá-lo caso necessário.

## Usos comuns

- monitorar uma fonte em vários dispositivos;
- enviar áudio para uma saída separada usada por outro aplicativo;
- monitorar áudio sem o atraso causado pela sincronização com vídeo;
- controlar separadamente o nível de cada fonte e dispositivo.

## Limitações conhecidas

- a estabilidade também depende do dispositivo, driver, frequência de
  amostragem e tamanho de buffer configurados no sistema;
- filtros de terceiros, como limiters e compressores, podem causar interrupções
  independentemente do Audio Monitor;
- esta rebuild não é uma versão oficial nem recebe suporte do autor original;
- no momento, a compilação disponibilizada por este fork é voltada ao macOS.

Ao relatar um problema, informe a versão do macOS e do OBS, os dispositivos de
saída utilizados, a frequência de amostragem e os filtros aplicados à fonte.

## Compilação

O procedimento geral acompanha o projeto original:

1. Compile o [OBS Studio](https://github.com/obsproject/obs-studio).
2. Coloque este repositório em `UI/frontend-plugins/audio-monitor`.
3. Adicione `add_subdirectory(audio-monitor)` ao arquivo
   `UI/frontend-plugins/CMakeLists.txt`.
4. Compile novamente o OBS Studio.

## Créditos e licença

Este fork é derivado do
[obs-audio-monitor](https://github.com/exeldro/obs-audio-monitor), criado e
mantido originalmente por [Exeldro](https://github.com/exeldro).

Os créditos do projeto original e sua licença permanecem preservados. Se o
plugin for útil para você, considere apoiar o trabalho do autor original:

- [GitHub Sponsors](https://github.com/sponsors/exeldro)
- [PayPal](https://www.paypal.me/exeldro)
- [Patreon](https://www.patreon.com/exeldro)

Rebuild e testes no macOS por
[@maurojuniorr](https://github.com/maurojuniorr).
