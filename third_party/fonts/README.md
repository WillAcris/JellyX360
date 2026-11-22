# Fonte de texto

O app usa a fonte **Terminus 10x20** (normal), de Dimitar Toshkov Zhekov,
licenciada sob a SIL Open Font License 1.1. O arquivo `ter-u20n.bdf` aqui já
foi ajustado com `DEFAULT_CHAR 0`, como o gerador do xsdk exige.

Ela tem exatamente a mesma célula da fonte antiga (10x20 px), então nenhum
layout da interface precisou mudar. `10x20-original.bdf` é a fonte original,
guardada apenas para rollback.

Para regerar o header usado pelo xsdk:

```sh
python3 /root/xsdk/tools/gen_font.py \
  --output /root/xsdk/src/text_font.h \
  ter-u20n.bdf
make -C /root/xsdk build/libxsdk.a
```

Fonte original: https://sourceforge.net/projects/terminus-font/files/
