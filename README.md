# 🎮 JellyX360 — Cliente Jellyfin para Xbox 360



## 📺 Sobre o Projeto

O **JellyX360** é um cliente não oficial do Jellyfin desenvolvido para o Xbox 360 com desbloqueio RGH/JTAG.

O projeto permite acessar bibliotecas de mídia e assistir a vídeos diretamente no console, utilizando o servidor Jellyfin para realizar a transcodificação do conteúdo para um formato compatível com o hardware do Xbox 360.

A reprodução utiliza **MPEG-1 SD (`mpeg1video` + `mp2`) em container MPEG-PS**, permitindo aproveitar um console clássico para acessar sua biblioteca de mídia pessoal.

### ✨ Características

- 🎮 **Desenvolvido para Xbox 360:** compatível com consoles desbloqueados RGH/JTAG.
- 📺 **Integração com Jellyfin:** acesso às bibliotecas e conteúdos do servidor.
- 🎬 **Reprodução de vídeos:** decodificação MPEG-1 e MP2 diretamente no console.
- 📡 **TV ao vivo:** interface dedicada para navegação entre canais.
- 🖼️ **Capas e miniaturas:** carregamento assíncrono com cache inteligente.
- 🎨 **Interface personalizada:** navegação adaptada ao controle do Xbox 360.
- ⚡ **Desempenho otimizado:** gerenciamento de buffers e carregamento em segundo plano.
- 💾 **Configuração persistente:** informações do servidor salvas no armazenamento do console.

## 📋 Funcionalidades

### 🎮 Navegação

- Home com abas de navegação.
- Bibliotecas organizadas em grades.
- Visualização de detalhes dos conteúdos.
- Navegação por canais de TV ao vivo.
- Grade de episódios.

### 🎬 Reprodução

- Transcodificação pelo servidor Jellyfin.
- Vídeo MPEG-1 em resolução SD.
- Áudio MP2.
- Player com barra de progresso.
- Indicador visual de pausa.
- OSD (On-Screen Display).

### 🖼️ Interface

- Home com abas destacadas e cards.
- TV ao vivo em grade de 6 × 3.
- Episódios e bibliotecas em grade de 4 × 3.
- Fonte Terminus 10×20.
- Carregamento gradual das capas.

## 📦 Arquivos Disponíveis

| Arquivo                | Descrição                                 |
| ---------------------- | ----------------------------------------- |
| `jellyx360.xex`        | Executável principal para o Xbox 360      |
| `jellyx360-stable.xex` | Cópia de segurança para rollback          |
| `jellyx360.log`        | Arquivo de logs gerado durante a execução |

**Identificação do aplicativo:**

- Nome: JellyX360
- TITLE_ID: `0x4A463336`
- Formato: `.xex`
- Versão: `0.2.0`

## 🚀 Instalação

### 1. Pré-requisitos

- Xbox 360 com desbloqueio RGH ou JTAG.
- Dashboard com suporte a execução de arquivos `.xex`.
- Servidor Jellyfin configurado e acessível pela rede.
- Conexão de rede entre o console e o servidor.
- Transcodificação MPEG-1/MP2 habilitada no Jellyfin.

### 2. Configurar o servidor Jellyfin

O cliente utiliza conexão HTTP.

Exemplo de endereço:

```text
http://192.168.1.100:8096
```

Substitua o endereço pelo IP local do seu servidor Jellyfin.

**Importante:** esta versão não oferece suporte a HTTPS. Utilize HTTP e a porta 8096.

### 3. Instalar no Xbox 360

1. Baixe o arquivo `jellyx360.xex` na seção de releases do GitHub.
2. Transfira o arquivo para o HD ou dispositivo de armazenamento do Xbox 360.
3. Inicie o console pelo XeXMenu ou pelo DashLaunch.
4. Execute o arquivo `jellyx360.xex`.
5. Informe os dados do seu servidor Jellyfin e faça login.

## 🎮 Controles

| Botão   | Função                                    |
| ------- | ----------------------------------------- |
| D-pad   | Navegação pelos menus                     |
| A       | Abrir, selecionar ou reproduzir           |
| B       | Voltar ou sair do aplicativo              |
| X       | Atualizar a tela atual                    |
| START   | Confirmar no login / abrir menu de perfil |
| LB / RB | Alternar abas na Home                     |

**Observações:**

- Na TV ao vivo e na grade de episódios, cima e baixo alternam entre linhas.
- No player, esquerda e direita não executam ações de busca nesta versão.

## ⚙️ Configurações e Compatibilidade

### Servidor Jellyfin

Parâmetros de transcodificação solicitados pelo cliente:

| Parâmetro           | Configuração |
| ------------------- | ------------ |
| Resolução           | 640 × 480    |
| Taxa de quadros     | Até 30 FPS   |
| Vídeo               | MPEG-1       |
| Bitrate de vídeo    | 2,5 Mbps     |
| Áudio               | MP2          |
| Bitrate de áudio    | 192 kbps     |
| Frequência de áudio | 48 kHz       |
| Canais de áudio     | 2            |
| Container           | MPEG-PS      |

O servidor pode fornecer outra taxa de amostragem de áudio, sendo o áudio reamostrado pelo cliente para 48 kHz.

### Armazenamento

A configuração do aplicativo fica em:

```text
game:/jellyx360.cfg
```

O aplicativo também reconhece automaticamente o arquivo antigo:

```text
game:/jellyfin360.cfg
```

Não é necessário realizar um novo login caso o arquivo antigo esteja disponível e válido.

### Gerenciamento de capas

- Download em thread separado.
- Fila de carregamento com 8 itens.
- Cache LRU com capacidade para 20 texturas.

## ⚠️ Limitações Conhecidas

- **HTTPS:** não suportado nesta versão. Utilize HTTP.
- **Busca durante a reprodução:** desativada no D-pad, pois o servidor testado reiniciava a transcodificação em vez de avançar 10 segundos.
- **Compatibilidade de transcodificação:** o servidor precisa aceitar MPEG-1, MP2 e container MPEG-PS.

Essas limitações poderão ser revistas em versões futuras.

## 🛠️ Desenvolvimento

### Pré-requisitos

- WSL2 com Ubuntu.
- CMake.
- Ninja.
- GCC Toolchain para Xbox 360 (`powerpc-xsdk-elf`).
- libxsdk.
- glslang.
- Dependências de compilação listadas abaixo.

### Dependências

```bash
build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev \
curl xz-utils patch python3 cmake ninja-build git libicu-dev
```

### Estrutura do ambiente

```text
/root/xsdk-gcc
/root/xsdk/build
/root/xsdk-gcc/bin/glslang
```

### Compilação

Copie o projeto para dentro do WSL2:

```bash
cp -a /mnt/c/Users/willa/Documents/Projeto\ Padrão/JellyX360 /root/jellyx360
```

Entre na pasta:

```bash
cd /root/jellyx360
```

Configure o CMake:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/root/xsdk/cmake/xsdk.cmake \
  -DXSDK_TOOLCHAIN=/root/xsdk-gcc \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Compile o projeto:

```bash
cmake --build build
```

O executável será gerado em:

```text
build/jellyx360.xex
```

**Observação:** o build utiliza `-Werror` apenas no código do projeto. Os componentes de `third_party` são incluídos como sistema.

### Regeneração da biblioteca de fontes

Caso a fonte seja modificada, regenere a biblioteca antes de compilar novamente:

```bash
make -C /root/xsdk build/libxsdk.a

cd /root/jellyx360
rm -f build/jellyx360.xex
cmake --build build
```

Mais informações sobre a fonte Terminus estão disponíveis em:

```text
third_party/fonts/README.md
```

## 🎨 Personalização da Interface

As configurações visuais ficam principalmente em:

```text
src/style.h
```

Esse arquivo controla:

- Cores.
- Espaçamentos.
- Escalas.
- Dimensões visuais.

A interface utiliza a fonte **Terminus 10×20**, licenciada sob SIL Open Font License 1.1.

### Regras para alterações visuais

Para preservar o funcionamento do aplicativo:

1. Alterações visuais devem modificar somente as funções `draw_*` e as primitivas em `src/ui.c`.
2. Não modificar funções `update_*`, o enum `Screen` ou campos de `App` durante ajustes exclusivamente visuais.
3. Executar o checklist do arquivo `SMOKE-TEST.md` após cada alteração.
4. Manter sempre uma cópia funcional de `jellyx360-stable.xex` para rollback.

## 🔧 Deploy no Console

1. Inicie o Xbox 360 com dashboard RGH/JTAG.
2. Habilite o servidor FTP do console.
3. Conecte-se ao console pela rede.
4. Copie `jellyx360.xex` para o HD.
5. Execute pelo XeXMenu ou DashLaunch.

## 📚 Notas Técnicas

- `pl_mpeg` realiza demux de MPEG-PS e decodifica MPEG-1 + MP2.
- Buffers de vídeo são preenchidos com `0xFF` antes de `plm_frame_to_argb`, pois o byte alpha não é escrito pela função.
- O formato resultante é A,R,G,B, compatível com `XSDK_FORMAT_A8R8G8B8`.
- Interface imediata com double buffering de vertex buffer e fences.
- `ui_end` é executado após o resolve.
- O clock de vídeo é ancorado no primeiro frame decodificado.
- A comunicação com o servidor utiliza HTTP puro.
- O token de autenticação é enviado no cabeçalho `Authorization: MediaBrowser`.
- `PlaySessionId` é utilizado para organizar a sessão de transcodificação.

## 📝 Histórico de Versões

Consulte o arquivo [CHANGELOG.md](CHANGELOG.md) para acompanhar as alterações, correções e novidades de cada versão.

## 🤝 Contribuição

Contribuições são bem-vindas!

O JellyX360 é um projeto em desenvolvimento e sugestões da comunidade ajudam a melhorar a experiência de uso no Xbox 360.

Você pode contribuir de várias formas:

1. 🐛 Reportar bugs e problemas.
2. 💡 Sugerir novas funcionalidades.
3. 🎨 Propor melhorias na interface.
4. ⚡ Sugerir otimizações de desempenho.
5. 🔧 Enviar correções e Pull Requests.
6. ⭐ Adicionar uma estrela ao repositório.

### Como contribuir

1. Faça um Fork do projeto.
2. Crie uma branch para sua alteração:

```bash
git checkout -b feature/MinhaMelhoria
```

3. Faça suas alterações.
4. Registre um commit:

```bash
git commit -m "Adiciona MinhaMelhoria"
```

5. Envie para o GitHub:

```bash
git push origin feature/MinhaMelhoria
```

6. Abra um Pull Request.

## 💰 Apoie o Projeto

Gostou do JellyX360 e quer ajudar no desenvolvimento?

Você pode apoiar o projeto por meio de doações voluntárias.

**Por que apoiar?**

- Incentiva a continuidade do desenvolvimento.
- Ajuda a financiar melhorias e novos recursos.
- Permite dedicar mais tempo à compatibilidade e otimização.
- Mantém vivo o desenvolvimento de software para consoles clássicos.



**Toda contribuição é voluntária e ajuda a manter o projeto ativo.**

## 📞 Contato e Suporte

Encontrou algum problema, tem uma ideia ou deseja contribuir?

Entre em contato pelos canais abaixo:

- 🐛 **Reportar problemas:** [GitHub Issues](https://github.com/WillAcris/JellyX360/issues)
- 💡 **Sugestões e melhorias:** [GitHub Discussions](https://github.com/WillAcris/JellyX360/discussions)
- 💻 **Código-fonte:** [GitHub Repository](https://github.com/WillAcris/JellyX360)
- 💰 **Doações:** [LivePix](https://livepix.gg/willacris)

## 📄 Licença

A licença do projeto deve ser consultada no arquivo [LICENSE](LICENSE).

---

<p align="center">
  Desenvolvido com carinho para a comunidade Xbox 360.
  <br>
  <strong>JellyX360</strong> — Seu Jellyfin no Xbox 360.
  <br>
  <sub>Preservando o console clássico com novas possibilidades.</sub>
</p>
