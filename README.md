# EnergyWise

ESP32-malli RJ12 HAN -mittariluentaan ja neljän GPIO-lähdön hintaohjaukseen. Selain näyttää osto- ja myyntitehon, ostoenergian, voimassa olevat hinnat ja lähtöjen tilat. Tehokuvaaja näyttää viimeiset noin 10 minuuttia 10 sekunnin välein. Mittauskatkot näkyvät aukkoina ja historia alkaa uudestaan laitteen käynnistyessä.

## Käännös ja käynnistys

1. Luo paikallinen `src/wifi.local.h` ja lisää testiverkon tunnukset. Tiedosto on Gitin ohituslistalla. Jos tiedosto puuttuu, laite käyttää ESP32:n aiemmin tallentamia Wi-Fi-tunnuksia.

   ```cpp
   #define ENERGYWISE_WIFI_SSID "testiverkko"
   #define ENERGYWISE_WIFI_PASSWORD "salasana"
   ```

2. Käännä `pio run` ja lataa testilaitteelle `pio run -t upload`.
3. Avaa `pio device monitor` ja selaa sarjaporttiin tulostuvaan IP-osoitteeseen. Käytä mallia eristetyssä testiverkossa. Käyttöliittymä ja API käyttävät HTTP:tä ilman kirjautumista.

## HAN ja lähdöt

Malli lukee UART2:n RX-tulosta GPIO16, 115200 baud, 8N1. Se odottaa DSMR/P1-tekstisanomaa, jossa on CRC-16-tarkiste. Tarkista mittarin dokumentaatiosta protokolla, RJ12:n pinnijärjestys, aktivointi, jännitetasot ja signaalin polariteetti. Käytä mittarille sopivaa HAN-sovitinta, jonka UART-lähtö on ESP32-yhteensopiva 3,3 V. RJ12:n pinnejä ei kytketä suoraan ESP32:een.

Lähdöt 1–4 ovat GPIO25, GPIO26, GPIO27 ja GPIO32. Mallin lähdöt ovat aktiivisia ylhäällä. Testaa ensin logiikkatasoilla. Verkkojännitekuormien kytkentä kuuluu sähköalan ammattilaiselle.

## Hintajaksojen lataus

Lataa hinnat käyttöliittymään JSON-tiedostona. Kukin rivi sisältää jakson alun Unix-sekunteina UTC-ajassa, pituuden minuutteina ja hinnan sentteinä kilowattitunnilta. Esimerkin aikaleimat ovat muotoesimerkkejä. Käytä ohjauksessa nykyisiä ja tulevia jaksoja.

```json
[
  {"start": 1767225600, "minutes": 60, "cents": 8.2},
  {"start": 1767225600, "minutes": 15, "cents": 7.1},
  {"start": 1767226500, "minutes": 15, "cents": -0.4}
]
```

Jokaiselle lähdölle voi valita tuntihinnan, varttihinnan tai pois-tilan ja oman hintarajan. Lähtö on päällä, kun valitun voimassa olevan jakson hinta on enintään raja. Ilman synkronoitua kelloa tai voimassa olevaa hintajaksoa lähtö pysyy pois päältä. Tunti- ja varttihinnat syötetään erillisinä jaksoina. Uusi hinnasto korvaa edellisen ja voi sisältää enintään 200 jaksoa. Laite tahdistaa kellon NTP:llä. Lähtöasetukset ja hinnasto säilyvät käynnistyksen yli. Tyhjä hinnasto (`[]`) poistaa tallennetut hinnat ja sammuttaa lähdöt.
