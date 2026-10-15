# Skate 3 Mobile no Redmi 9 (Android 12 / Mali-G52)

Este documento cobre o branch `redmi9-android12` deste fork: o que foi mudado,
como compilar, instalar e testar, e o que ainda falta. **Nada aqui foi testado
em um Redmi 9 de verdade ainda.** A seção "Relatório de testes" é um template
para você preencher.

## 1. Análise de viabilidade

### Hardware (Redmi 9)

| Item | Redmi 9 | RG406V (referência do upstream) |
|---|---|---|
| SoC | MediaTek Helio G80 (12 nm) | Unisoc T820 (6 nm) |
| CPU | 2x Cortex-A75 (2,0 GHz) + 6x Cortex-A55 (1,8 GHz) | 1x A76 2,7 + 3x A76 2,3 + 4x A55 2,1 GHz |
| GPU | Mali-G52 MC2 | Mali-G57 MC4 |
| RAM | 3, 4 ou 6 GB | 4 a 8 GB |
| Android | 10 de fábrica, até Android 12 / MIUI 13 | 13 |

Fontes: [GSMArena](https://www.gsmarena.com/xiaomi_redmi_9-10233.php),
[Notebookcheck Helio G80](https://www.notebookcheck.net/Mediatek-Helio-G80-Processor-Benchmarks-and-Specs.466931.0.html),
[Notebookcheck T820](https://www.notebookcheck.com/UNISOC-T820-Prozessor-Benchmarks-und-Specs.644931.0.html).

### O que o upstream exige e o que isso realmente significa

| Requisito declarado | Origem no código | Veredito |
|---|---|---|
| Android 13 (API 33) | `minSdkVersion 33`, preset `android-33`, checagem no launcher | **Removível.** Só uma chamada exige API 33 (ver abaixo). |
| ARMv8.2 com FP16 e dot-product | Só aparece no README | **Não é exigência do binário.** CMake e SDK compilam com `-march=armv8-a`. O A75 do Helio G80 é ARMv8.2, mas isso não importa para o build. |
| Vulkan | `uses-feature vulkan.level` e checagem do launcher | Atendido pelo G52, mas ver riscos. |
| ~8 GiB livres | Jogo extraído tem ~6 GiB | Inalterado. |

A única chamada que quebra no Android 12 é
`Context.registerReceiver(receiver, filter, int flags)` em
`HIDDeviceManager.java` (API 33). Sem proteção, o jogo cairia com
`NoSuchMethodError` ao abrir a activity. Foi corrigido com um wrapper que usa a
chamada de 2 argumentos abaixo da API 33. O resto do código Java já tinha
guardas de versão (SDL).

### Conclusão

**Rodar no Android 12 é viável com mudanças pequenas.** O risco real é
**desempenho**, não compatibilidade: o Redmi 9 tem metade dos núcleos de GPU e
só dois núcleos de CPU rápidos, e o jogo é uma recompilação do Xbox 360 com
emulação de GPU. O upstream só verificou o RG406V.

### Riscos e bloqueios possíveis (não verificados no aparelho)

1. **Vulkan no Mali-G52 do Helio G80.** O driver pode reportar menos extensões
   ou recursos que o Mali-G57. O SDK exige `independentBlend` e
   `fragmentStoresAndAtomics`, e Vulkan 1.1+ para várias extensões opcionais.
   Se o driver não oferecer `fragmentStoresAndAtomics`, o jogo **não inicia** e
   não há contorno simples. Verifique com um app como "Vulkan Caps Viewer"
   antes de gastar tempo.
2. **Memória.** Variantes de 3 GB ficam no limite. Abaixo de ~4 GB de RAM o app
   já usa budgets menores de textura e malha; a variante de 6 GB usa os budgets
   padrão (ver "O que falta").
3. **30 FPS pode não ser atingido.** 60 FPS é improvável neste hardware.
4. **MIUI** pode matar processos em segundo plano e limitar CPU; ver seção de
   instalação.
5. **Armazenamento.** Extrair 6 GiB para a pasta do app em um aparelho de
   32 GB cheio falha por falta de espaço.

## 2. O que foi alterado

### Android 12 sem quebrar Android 13+

- `android/app/build.gradle`: `minSdkVersion 33` -> `31`. `targetSdkVersion`
  continua 35.
- `CMakePresets.json`: `ANDROID_PLATFORM` `android-33` -> `android-31`.
- `HIDDeviceManager.java`: `registerReceiver` com versão protegida por
  `Build.VERSION.SDK_INT`. Em Android 13+ o comportamento é idêntico ao antigo.
- `LauncherActivity.java` e `LauncherStrings.java`: checagem e textos agora
  dizem Android 12+ (inclui tradução pt-BR).
- `AppUpdater.java`: abaixo do Android 13 o app **não oferece** as atualizações
  do upstream, cujos APKs exigem API 33 e não instalariam.

### Perfil "Redmi 9 / Mali-G52 / Android 12"

Implementado em `src/skate3_android_defaults.cpp`. É detectado automaticamente
(propriedades `ro.board.platform`, `ro.hardware`, `ro.soc.model`,
`ro.product.board` contendo `mt6768` ou `mt6769`, que cobre Helio G70, G80 e
G85). Não depende de RAM, porque um Redmi 9 de 6 GB seria tratado como
aparelho "padrão" e cairia no perfil 720p.

Quando detectado:

| Ajuste | Valor | Motivo |
|---|---|---|
| Perfil gráfico | RG406V / Performance (índice 0) | 512x288 interno, HUD em resolução total, LOD e distância 0,5x, materiais simplificados, grama e pós-processamento caros desligados, oclusão ligada |
| Limite de FPS | 30 | Ritmo estável é melhor que 40-50 irregular com 2 núcleos rápidos |
| Buffer de áudio | 1024 frames | 512 pode picotar com os 2 núcleos saturados |
| Mips base de textura e refresh estático | valores dos aparelhos < 4 GB | Menos memória e menos trabalho por quadro |
| Afinidade de threads | `Main XThread` e `GPU Commands` cada um em um núcleo rápido; streaming nos núcleos lentos | O mapa original colocava 6 threads quentes nos 2 núcleos rápidos |
| Driver Vulkan | System Driver | Turnip é só para Adreno; nada foi alterado aqui |

O menu continua mandando: configurações salvas carregam depois dos defaults, e
**RB + Start > Video > Android Device Profile** troca o perfil.

Controles de teste, sem recompilar:

```sh
adb shell setprop debug.skate3.weak_gpu 1    # força o perfil em outro Mali-G52 (0 desliga)
adb shell setprop debug.skate3.perf_log 1    # estatísticas de quadro a cada 300 quadros
```

### Garantia de não-regressão

Os ajustes só valem quando o perfil é detectado. Em simulação no host, com
topologias de CPU e propriedades fictícias, um RG406V e um aparelho de 8 núcleos
genérico continuam com perfil 1, 60 FPS, áudio 512 e o mapa de threads original.
O teste do upstream `tests/rp6/check-updates.py` continua passando.

## 3. Build

O build **precisa dos seus arquivos do jogo**, porque o recompilador gera código
a partir deles. O repositório não contém nada retail. Você fornece o ISO
extraído e o Title Update 3.

### Pelo GitHub Actions

Workflow: `.github/workflows/build-android.yml` (disparo manual).

1. Reúna, nesta estrutura:

   ```text
   game/default.xex
   game/data/webkit/EAWebkit.xex
   TU_12K2276_000000C000000.00000000000O3
   ```

2. Escolha **uma** forma de entregar isso ao workflow:
   - **Runner próprio (macOS ARM):** defina a variável de repositório
     `SKATE3_INPUT_DIR` apontando para a pasta acima.
   - **Runner hospedado:** gere um `.tar.gz` com essa estrutura, hospede em um
     endereço https **privado** e salve em `SKATE3_INPUTS_URL` (secret).
     Opcionalmente salve o SHA-256 em `SKATE3_INPUTS_SHA256`.
3. Em **Actions > Build Android APK > Run workflow** use `macos-14`.
4. Baixe o artefato `skate3-mobile-android` (APK e `SHA256SUMS.txt`).

Assinatura (opcional): sem ela o APK sai com chave de debug, o que basta para
instalar no seu aparelho. Para assinar, marque `signed` e crie os secrets
`SKATE3_RELEASE_KEYSTORE_B64` (keystore em base64),
`SKATE3_RELEASE_STORE_PASSWORD`, `SKATE3_RELEASE_KEY_ALIAS` e
`SKATE3_RELEASE_KEY_PASSWORD`.

Importante: o APK contém código gerado a partir de arquivos retail. Mantenha o
repositório privado ou não publique o artefato. O artefato expira em 7 dias.

Há também `.github/workflows/android-java-check.yml`, que **não usa arquivos do
jogo**: roda o teste de política de atualização, compila o lado Java com
minSdk 31, roda o lint e falha se houver chamadas a API mais nova que o
Android 12.

### Localmente (macOS Apple Silicon)

Como no upstream: `./build-android.sh --game-dir ... --title-update ...`. Veja
`android/README.md`.

## 4. Instalação no Redmi 9

1. Habilite instalar apps de fontes desconhecidas para o app que abrir o APK.
2. Instale `Skate3-Mobile-Android-debug.apk`. O pacote é `chat.buku.skate3.dev`.
3. Abra o app, escolha **Select My Skate 3 ISO** e aponte para o seu ISO.
   Mantenha o app aberto durante os ~6 GiB de extração. Tenha ~8 GiB livres, e
   ~15 GiB se o ISO estiver no armazenamento interno.
4. **Play Skate 3**.

Dicas para MIUI: em Configurações > Apps > Skate 3 > Economia de bateria,
escolha "Sem restrições"; fixe o app nos recentes; use o modo desempenho.

## 5. Limitações conhecidas

- Nenhum teste em aparelho real foi feito por quem escreveu este branch.
- 60 FPS não é esperado. A meta realista é um 30 estável.
- Vulkan no Mali-G52 pode ter artefatos visuais ou falhas de driver.
- Aparelhos de 3 GB podem ter travamentos por memória.
- As atualizações automáticas do app estão desligadas abaixo do Android 13.
  Atualize instalando o novo APK por cima.
- O botão de bug report ainda aponta para o repositório do upstream.

## 6. Otimizações aplicadas e o que falta

Aplicadas:

- Perfil Performance automático (288p interno, LOD e distância 0,5x, materiais
  simples, sem grama, SSAO, bloom, volumetria; oclusão ligada).
- Teto de 30 FPS com pacing do guest.
- Buffer de áudio de 1024 frames.
- Mips base reduzidos para este perfil. Budgets de textura e malha menores
  (128 MB / 96 MB) valem só abaixo de ~4 GB de RAM; a variante de 6 GB usa
  288 / 224 MB.
- Afinidade de threads pensada para 2 núcleos rápidos.
- System Driver mantido; não há Turnip em Mali.

Ainda falta (precisa de medição em aparelho):

- Medir quadros, p95 e uso de CPU com `debug.skate3.perf_log` e ajustar o
  limite de 30 FPS, a escala 512x288 e o buffer de áudio.
- Cache de pipelines Vulkan persistente para reduzir engasgos de shader.
- Revisar shaders caros no G52 (precisão `mediump`, menos branches).
- Avaliar texturas comprimidas de formato nativo do Mali.
- Opção de resolução interna ainda menor (ex.: 384x216).
- Decidir se a variante de 6 GB também deve usar os budgets pequenos.
- Revisar o carregamento (streaming) no armazenamento eMMC/UFS do Redmi 9.
- Gate de `fragmentStoresAndAtomics` com mensagem clara no launcher.

## 7. Relatório de testes (preencher)

Aparelho: Redmi 9 ______ GB RAM / Android 12 / MIUI ____ / build ______
Driver Vulkan: System Driver / versão ______

| Área | Resultado | Observações |
|---|---|---|
| Instalação do APK | | |
| Launcher abre | | |
| Extração do ISO e TU3 | | |
| Boot do jogo | | |
| Menus | | |
| Input (controle / toque) | | |
| Renderização Vulkan | | |
| Áudio | | |
| Gameplay | | |
| FPS médio / p95 (`perf_log`) | | |
| Temperatura / throttling após 15 min | | |
| Crashes (logcat) | | |

Para coletar logs: `adb logcat -d > redmi9.log` e, se o jogo cair, anexe-o a
um issue.

## 8. Autoria e licenças

O histórico git do upstream foi preservado; este branch apenas adiciona
commits. Licenças, avisos (`docs/THIRD_PARTY_NOTICES.md`) e créditos do README
continuam valendo. Não há arquivos retail no repositório.
