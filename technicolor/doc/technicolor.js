/*
var p = 1;
var maxBorder = 5;
var loopCount = 1;
var interval = 40;
var goingUp = true;
function updateborder() {
  document.body.style.border = "" + p + "px dashed white";
  document.body.style.padding = ((12 + maxBorder - p) + "px");
  if (goingUp) {
    p++;
    if (p == maxBorder) {
      goingUp = false;
    }
  } else {
    p--;
    if (p == 0) {
      goingUp = true;
      loopCount--;
    }
  }
  if (loopCount > 0) {
    setTimeout("updateborder();", interval);
  }
}
function init() {
  setTimeout("updateborder();", interval);
}
*/


var foo = 1;

var starting_colors = new Array();
