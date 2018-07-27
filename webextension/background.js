'use strict';

async function http_request(url) {
    return new Promise(resolve => {
        var xmlHttp = new XMLHttpRequest();
        xmlHttp.onreadystatechange = function() { 
            if (xmlHttp.readyState == 4)
                resolve({status: xmlHttp.status, text: xmlHttp.responseText});
        }
        xmlHttp.open("GET", url, true); // true for asynchronous 
        xmlHttp.send(null);
    });
}

function sleep(ms) {
  return new Promise(resolve => { setTimeout(() => resolve(), ms); });
}

function enable_ouinet_client_proxy() {
    let proxySettings = {
      proxyType: "manual",
      http: "127.0.0.1:8080",
      ssl:  "127.0.0.1:8080",
    };
    
    browser.proxy.settings.set({value: proxySettings});
}

function disable_ouinet_client_proxy() {
    browser.proxy.settings.set({value: { proxyType: "manual" }});
}

function on_ouinet_enabled() {
    //console.log("on_ouinet_enabled");
    enable_ouinet_client_proxy();
    browser.browserAction.setTitle({ title: "Ouinet (running)"});
    browser.browserAction.setIcon({ path: { 24: "enabled.png" } });
}

function on_ouinet_disabled() {
    //console.log("on_ouinet_disabled");
    disable_ouinet_client_proxy();
    browser.browserAction.setTitle({ title: "Ouinet (not running)"});
    browser.browserAction.setIcon({ path: { 24: "disabled.png" } });
}

async function main() {
    var frontend_url = "http://localhost:8080";
    var marked_running = true;

    on_ouinet_disabled();

    while (true) {
        var r = await http_request(frontend_url);

        if (!marked_running && r.status == 200) {
            marked_running = true;
            on_ouinet_enabled();
        }
        else if (marked_running && r.status != 200) {
            marked_running = false;
            on_ouinet_disabled();
        }

        await sleep(1000);
    }
}

browser.browserAction.onClicked.addListener((tab) => {
    // TODO: Firebase Dynamic Links:
    // https://firebase.google.com/docs/dynamic-links/create-manually
    // var url = "https://equalitie.page.link/?link=https://equalit.ie&apn=ie.equalit.ouinet.service";

    browser.tabs.create({
        url: "ouinet://control"
    });

});

main().then(() => console.log("Done"));
