# BatteryDock

Monitor leve de bateria para Windows, escrito em C++ nativo com APIs Win32. Exibe ícones de dispositivos e níveis de bateria junto à área de notificação da barra de tarefas.

## Recursos

- Widget compacto preso à janela da barra de tarefas, ao lado dos ícones de notificação. Fica recortado pelos limites da barra e usa os glifos de headset, mouse, teclado e controle do Windows; o fone usa o mesmo glifo `Headphone` do Wireless Battery.
- O widget se oculta automaticamente durante aplicativos em tela cheia e reaparece ao voltar para a área de trabalho.
- Menu para mostrar ou ocultar dispositivos, escolher barra de tarefas ou bandeja, atualizar, iniciar com o Windows e sair.
- Notificação quando um aparelho chega a 15% ou menos.
- Leitura contínua do Sony PULSE 3D pelo dongle USB, sem enviar comandos ao headset.
- Detecção de controles sem fio compatíveis com XInput.
- Leitura de dispositivos HID que implementam o uso padrão Battery Strength.
- Integração opcional com HeadsetControl para headsets compatíveis.

## Compatibilidade

| Provedor | Aparelhos | Observação |
| --- | --- | --- |
| Sony HID | PULSE 3D pelo dongle USB | Atualiza quando o headset envia relatório; o nível é reportado em passos de 10%. |
| XInput | Controles Xbox sem fio compatíveis | O Windows fornece faixas (vazio, baixo, médio ou cheio), não um percentual exato; o valor aparece com `~`. |
| HID padrão | Dispositivos com Battery Strength | Depende de o aparelho anunciar o uso padrão no relatório HID. |
| HeadsetControl | Headsets listados pelo HeadsetControl | Opcional. Deixe `headsetcontrol.exe` ao lado do BatteryDock ou no `PATH`; a consulta ocorre uma vez por minuto. |

Não há uma API universal para a bateria de todos os acessórios. Muitos mouses, teclados e headsets usam protocolos próprios do fabricante. Bluetooth LE GATT e esses protocolos específicos ainda não estão incluídos; aparelhos sem um provedor compatível não terão um percentual inventado.

## Executar

Baixe `BatteryDock.exe` na página **Releases** e abra-o. O programa fica na área de notificação. Clique com o botão direito no widget ou no ícone da bandeja para abrir o menu. A opção `Start with Windows` registra o programa para a sessão atual do usuário.

## Compilar no Windows

1. Instale o [MSYS2](https://www.msys2.org/) e o ambiente UCRT64 com `g++`.
2. Abra um terminal do Windows e adicione `C:\msys64\ucrt64\bin` ao `PATH`.
3. Na pasta do projeto, execute:

   ```bat
   build.bat
   ```

O script gera `BatteryDock.exe` na pasta do projeto. Ele usa MinGW-w64 e liga estaticamente o runtime do C++, então não precisa de DLLs do MSYS2 para executar.

## Estrutura

```text
BatteryDock/
├── src/
│   └── main.cpp       # interface Win32, widget, menu e provedores de bateria
├── .gitignore
├── build.bat          # compilação MinGW-w64 UCRT64
├── LICENSE
└── README.md
```

## Contribuir

Para adicionar suporte a um aparelho, implemente um provedor que só reporte bateria quando houver uma leitura identificável e documente limitações e método de leitura. Para protocolos HID, não envie comandos de saída sem necessidade: este monitor deve continuar passivo sempre que possível.

## Licença

Distribuído sob a licença MIT. Consulte [LICENSE](LICENSE).

