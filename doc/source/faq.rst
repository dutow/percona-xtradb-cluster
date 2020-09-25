.. _faq:

==========================
Frequently Asked Questions
==========================

<<<<<<< HEAD
.. contents::
  :local:
  :backlinks: none
  :depth: 1

How do I report bugs?
=====================

All bugs can be reported on
`JIRA <https://jira.percona.com/projects/PXC/issues>`_.
Please submit ``error.log`` files from **all** the nodes.

How do I solve locking issues like auto-increment?
==================================================

For auto-increment, |PXC| changes ``auto_increment_offset`` for each new node.
In a single-node workload, locking is handled in the same way as |InnoDB|.
In case of write load on several nodes, |PXC| uses `optimistic locking <http://en.wikipedia.org/wiki/Optimistic_concurrency_control>`_
and the application may receive lock error in response to ``COMMIT`` query.

What if a node crashes and InnoDB recovery rolls back some transactions?
========================================================================

When a node crashes, after restarting,
it will copy the whole dataset from another node
(if there were changes to data since the crash).

How can I check the Galera node health?
=======================================

To check the health of a Galera node, use the following query:

.. code-block:: mysql

   SELECT 1 FROM dual;

The following results of the previous query are possible:

* You get the row with ``id=1`` (node is healthy)

* Unknown error
  (node is online, but Galera is not connected/synced with the cluster)

* Connection error (node is not online)

You can also check a node's health with the ``clustercheck`` script.
First set up the ``clustercheck`` user:

.. code-block:: mysql

   mysql> CREATE USER 'clustercheck'@'localhost' IDENTIFIED BY PASSWORD
   '*2470C0C06DEE42FD1618BB99005ADCA2EC9D1E19';
   Query OK, 0 rows affected (0.00 sec)

   mysql> GRANT PROCESS ON *.* TO 'clustercheck'@'localhost';

.. **

You can then check a node's health by running the ``clustercheck`` script:

.. code-block:: bash

   /usr/bin/clustercheck clustercheck password 0

If the node is running, you should get the following status: ::

  HTTP/1.1 200 OK
  Content-Type: text/plain
  Connection: close
  Content-Length: 40

  Percona XtraDB Cluster Node is synced.

In case node isn't synced or if it is offline, status will look like: ::

  HTTP/1.1 503 Service Unavailable
  Content-Type: text/plain
  Connection: close
  Content-Length: 44

  Percona XtraDB Cluster Node is not synced.

.. note::

   The ``clustercheck`` script has the following syntax:

   ``<user> <pass> <available_when_donor=0|1> <log_file> <available_when_readonly=0|1> <defaults_extra_file>``

   Recommended: ``server_args = user pass 1 /var/log/log-file 0 /etc/my.cnf.local``

   Compatibility: ``server_args = user pass 1 /var/log/log-file 1 /etc/my.cnf.local``

How does Percona XtraDB Cluster handle big transactions?
========================================================

|PXC| populates write set in memory before replication,
and this sets the limit for the size of transactions that make sense.
There are wsrep variables for maximum row count
and maximum size of write set
to make sure that the server does not run out of memory.

Is it possible to have different table structures on the nodes?
===============================================================

For example, if there are four nodes, with four tables:
``sessions_a``, ``sessions_b``, ``sessions_c``, and ``sessions_d``,
and you want each table in a separate node,
this is not possible for InnoDB tables.
However, it will work for MEMORY tables.

What if a node fails or there is a network issue between nodes?
===============================================================

The quorum mechanism in |PXC| will decide which nodes can accept traffic
and will shut down the nodes that do not belong to the quorum.
Later when the failure is fixed,
the nodes will need to copy data from the working cluster.

The algorithm for quorum is Dynamic Linear Voting (DLV).
The quorum is preserved if (and only if) the sum weight of the nodes
in a new component strictly exceeds half that
of the preceding Primary Component,
minus the nodes which left gracefully.

The mechanism is described in detail in `Galera documentation
<http://galeracluster.com/documentation-webpages/weightedquorum.html>`_.

How would the quorum mechanism handle split brain?
==================================================

The quorum mechanism cannot handle split brain.
If there is no way to decide on the primary component,
|PXC| has no way to resolve a |split brain|.
The minimal recommendation is to have 3 nodes.
However, it is possibile to allow a node to handle traffic
with the following option: ::

  wsrep_provider_options="pc.ignore_sb = yes"

Why a node stops accepting commands if the other one fails in a 2-node setup?
=============================================================================

This is expected behavior to prevent |split brain|.
For more information, see previous question or `Galera documentation
<http://galeracluster.com/documentation-webpages/weightedquorum.html>`_.

Is it possible to set up a cluster without state transfer?
==========================================================

It is possible in two ways:

1. By default, Galera reads starting position
   from a text file :file:`<datadir>/grastate.dat`.
   Make this file identical on all nodes,
   and there will be no state transfer after starting a node.

2. Use the :variable:`wsrep_start_position` variable to start the nodes
   with the same ``UUID:seqno`` value.

What TCP ports are used by Percona XtraDB Cluster?
==================================================

You may need to open up to four ports if you are using a firewall:

1. Regular MySQL port (default is 3306).

2. Port for group communication (default is 4567).
   It can be changed using the following option: ::

     wsrep_provider_options ="gmcast.listen_addr=tcp://0.0.0.0:4010; "

3. Port for State Snaphot Transfer (default is 4444).
   It can be changed using the following option: ::

     wsrep_sst_receive_address=10.11.12.205:5555

4. Port for Incremental State Transfer
   (default is port for group communication + 1 or 4568).
   It can be changed using the following option: ::

     wsrep_provider_options = "ist.recv_addr=10.11.12.206:7777; "

Is there "async" mode or only "sync" commits are supported?
===========================================================

|PXC| does not support "async" mode, all commits are synchronous on all nodes.
To be precise, the commits are "virtually" synchronous,
which means that the transaction should pass *certification* on nodes,
not physical commit.
Certification means a guarantee that the transaction does not have conflicts
with other transactions on the corresponding node.

Does it work with regular MySQL replication?
============================================

Yes. On the node you are going to use as source,
you should enable ``log-bin`` and ``log-slave-update`` options.

Why the init script (/etc/init.d/mysql) does not start?
=======================================================

Try to disable SELinux with the following command:

.. code-block:: bash

  echo 0 > /selinux/enforce

What does "nc: invalid option -- 'd'" in the sst.err log file mean?
===================================================================

This error is specific to Debian and Ubuntu.  |PXC| uses ``netcat-openbsd``
package. This dependency has been fixed.  Future releases of |PXC| will be
compatible with any ``netcat`` (see bug :jirabug:`PXC-941`).

||||||| 5b5a5d2584a
Q: Will |Percona Server| with |XtraDB| invalidate our |MySQL| support?
======================================================================

A: We don't know the details of your support contract. You should check with
your *Oracle* representative. We have heard anecdotal stories from |MySQL|
Support team members that they have customers who use |Percona Server| with
|XtraDB|, but you should not base your decision on that.

Q: Will we have to *GPL* our whole application if we use |Percona Server| with |XtraDB|?
========================================================================================

A: This is a common misconception about the *GPL*. We suggest reading the *Free
Software Foundation* 's excellent reference material on the `GPL Version 2
<http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>`_, which is the license
that applies to |MySQL| and therefore to |Percona Server| with |XtraDB|. That
document contains links to many other documents which should answer your
questions. |Percona| is unable to give legal advice about the *GPL*.

Q: Do I need to install |Percona| client libraries?
===================================================

A: No, you don't need to change anything on the clients. |Percona Server| is
100% compatible with all existing client libraries and connectors.

Q: When using the |Percona XtraBackup| to setup a replication slave on Debian based systems I'm getting: "ERROR 1045 (28000): Access denied for user 'debian-sys-maint'@'localhost' (using password: YES)"
==========================================================================================================================================================================================================

A: In case you're using init script on |debian| based system to start ``mysqld``,
be sure that the password for ``debian-sys-maint`` user has been updated and
it's the same as that user's password from the server that the backup has been
taken from. The password can be seen and updated in
:file:`/etc/mysql/debian.cnf`. For more information on how to set up a
replication slave using |Percona XtraBackup| see `this how-to
<http://www.percona.com/doc/percona-xtrabackup/2.1/howtos/setting_up_replication.html>`_.

.. include:: .res/replace.txt
=======
Q: Will |Percona Server| with |XtraDB| invalidate our |MySQL| support?
======================================================================

A: We don't know the details of your support contract. You should check with
your *Oracle* representative. We have heard anecdotal stories from |MySQL|
Support team members that they have customers who use |Percona Server| with
|XtraDB|, but you should not base your decision on that.

Q: Will we have to *GPL* our whole application if we use |Percona Server| with |XtraDB|?
========================================================================================

A: This is a common misconception about the *GPL*. We suggest reading the *Free
Software Foundation* 's excellent reference material on the `GPL Version 2
<http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>`_, which is the license
that applies to |MySQL| and therefore to |Percona Server| with |XtraDB|. That
document contains links to many other documents which should answer your
questions. |Percona| is unable to give legal advice about the *GPL*.

Q: Do I need to install |Percona| client libraries?
===================================================

A: No, you don't need to change anything on the clients. |Percona Server| is
100% compatible with all existing client libraries and connectors.

Q: When using the |Percona XtraBackup| to setup a replication replica on Debian based systems I'm getting: "ERROR 1045 (28000): Access denied for user 'debian-sys-maint'@'localhost' (using password: YES)"

A: In case you're using init script on |debian| based system to start ``mysqld``,
be sure that the password for ``debian-sys-maint`` user has been updated and
it's the same as that user's password from the server that the backup has been
taken from. The password can be seen and updated in
:file:`/etc/mysql/debian.cnf`. For more information on how to set up a
replication replica using |Percona XtraBackup| see `this how-to
<http://www.percona.com/doc/percona-xtrabackup/2.1/howtos/setting_up_replication.html>`_.

.. include:: .res/replace.txt
>>>>>>> ps/release-8.0.21-12
