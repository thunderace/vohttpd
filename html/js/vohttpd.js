function vohttpd_message(title, text, id, btn1, p1, btn2, p2) {
    if(id === null)
        id = "vohttpd-message";
    $("#" + id).remove();

    var html = "";
    html += "<div id=\"" + id + "\" class=\"modal fade\">";
    html += "<div class=\"modal-dialog\"><div class=\"modal-content\">";

    html += "<div class=\"modal-header\">";
    html += "<button type=\"button\" class=\"close\" data-dismiss=\"modal\" aria-hidden=\"true\">&times;</button>";
    html += "<h4 class=\"modal-title\">" + title + "</h4>";
    html += "</div>";

    html += "<div class=\"modal-body\">";
    html += "<p>" + text + "</p>";
    html += "<div style=\"border-top:0px\" class=\"modal-footer\">";

    if(btn1 && p1)
        html += "<button " + p1 + " type=\"button\" class=\"btn btn-primary\" data-dismiss=\"modal\">" + btn1 + "</button>";
    if(btn2 && p2)
        html += "<button " + p2 + " type=\"button\" class=\"btn btn-primary\" data-dismiss=\"modal\">" + btn2 + "</button>";

    html += "<button type=\"button\" class=\"btn btn-default\" data-dismiss=\"modal\">Close</button></div>";
    html += "</div></div></div>";

    $(document.body).append(html);
    $("#" + id).modal("show");
}

function vohttpd_call(name, param) {
    var raw;
    if(param === null)
        raw = $.ajax({url:"/cgi-bin/" + name, async:false});
    else
        raw = $.ajax({url:"/cgi-bin/" + name + "?" + param, async:false});
    return $.parseJSON(raw.responseText);
}

function vohttpd_create_panel(title, id, id_body) {
    var html = "";
    html += "<div class=\"container\"><div class=\"panel panel-info\" id=\"" + id + "\">";
    html += "<div class=\"panel-heading\"><h3 class=\"panel-title\">" + title + "</h3></div>";
    html += "<div class=\"panel-body\" id=\"" + id_body + "\"></div></div>";
    $(document.body).append(html);
}

function vohttpd_file_list(path) {
    var html = $.ajax({url:path, async:false}).responseText;
    var b = 0, e = 0, btag = "<p id=\"file\">", etag="</p>";
    var array = [];
    for(;;) {
        b = html.indexOf(btag, b);
        if(b < 0) break;
        e = html.indexOf(etag, b);
        if(e < 0 || e < b) break;
        array.push(html.substr(b + btag.length, e - b - btag.length));
        b = e + etag.length;
    }
    return array;
}

function vohttpd_main() {
    var list = vohttpd_file_list("js/plugin");
    for(i = 0; i < list.length; i++)
        $.getScript("js/plugin/" + list[i]);
}

$(document).ready(function(){vohttpd_main();});
