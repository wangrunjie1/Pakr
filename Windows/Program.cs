using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using System.Xml.Linq;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        try
        {
            var config = XElement.Load(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "app-config.xml"));
            var url = new Uri((string)config.Element("Url"));
            var title = (string)config.Element("Name");
            var appId = (string)config.Element("Id");
            using (var window = new Form { Text = title, Size = new Size(1100, 760), StartPosition = FormStartPosition.CenterScreen })
            using (var web = new WebView2 { Dock = DockStyle.Fill })
            using (var icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath))
            {
                window.Icon = icon;
                window.Controls.Add(web);
                window.Shown += async (sender, args) =>
                {
                    try
                    {
                        var profile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Pakr", appId, "WebView2");
                        var environment = await CoreWebView2Environment.CreateAsync(userDataFolder: profile);
                        if (window.IsDisposed) return;
                        await web.EnsureCoreWebView2Async(environment);
                        if (window.IsDisposed) return;
                        var browser = web.CoreWebView2;
                        browser.Settings.AreHostObjectsAllowed = false;
                        browser.Settings.IsWebMessageEnabled = false;
                        browser.NavigationStarting += (_, navigation) =>
                        {
                            navigation.Cancel = !IsHttps(navigation.Uri);
                        };
                        browser.NewWindowRequested += (_, popup) =>
                        {
                            popup.Handled = true;
                            if (IsHttps(popup.Uri)) browser.Navigate(popup.Uri);
                        };
                        browser.PermissionRequested += (_, permission) => permission.State = CoreWebView2PermissionState.Deny;
                        browser.Navigate(url.AbsoluteUri);
                    }
                    catch (WebView2RuntimeNotFoundException)
                    {
                        MessageBox.Show(window, "This app requires Microsoft Edge WebView2 Runtime. Install it from Microsoft, then reopen the app.", title);
                        window.Close();
                    }
                    catch (Exception)
                    {
                        if (window.IsDisposed) return;
                        MessageBox.Show(window, "The embedded browser could not start. Reopen the app and try again.", title);
                        window.Close();
                    }
                };
                Application.Run(window);
            }
        }
        catch (Exception)
        {
            MessageBox.Show("The app configuration could not be read. Reinstall the app and try again.", "Pakr Windows");
        }
    }

    private static bool IsHttps(string value)
    {
        return Uri.TryCreate(value, UriKind.Absolute, out var uri)
            && uri.Scheme == Uri.UriSchemeHttps && string.IsNullOrEmpty(uri.UserInfo);
    }
}
